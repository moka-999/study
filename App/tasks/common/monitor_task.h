/* 系统监测任务对未来PC–MCU JSON业务层提供的快照。 */
#ifndef MONITOR_TASK_H
#define MONITOR_TASK_H

#include "memory_monitor.h"
#include "mcp4725.h"
#include "pwr_ctrl.h"
#include <stdint.h>

typedef struct {
    uint32_t timestamp_ms;
    pwr_ctrl_state_t power_state;
    memory_monitor_sample_t memory;
} monitor_snapshot_t;

typedef enum {
    MONITOR_BOOT_OK = 0,
    MONITOR_BOOT_ERR_PARAM = -1,
    MONITOR_BOOT_ERR_STATE = -2,
    MONITOR_BOOT_ERR_BUSY = -3,
    MONITOR_BOOT_ERR_IO = -4,
    MONITOR_BOOT_ERR_NOT_SET = -5,
} monitor_boot_result_t;

/*
 * 任务上下文复制最新快照：1=存在快照，0=尚无快照或输出指针为空。
 * 输出由调用方持有，不暴露任务内部缓冲；临界区只包围结构复制。
 * “有快照”不等于测量有效：必须检查power_state、memory.valid_mask和
 * timestamp_ms的新鲜度。不会触发即时硬件采样，也不等待下次采样。
 */
int monitor_get_latest(monitor_snapshot_t *snapshot);

/*
 * 调度前创建内存器件访问互斥量；成功返回1。监测任务和PC调压命令共用它，
 * 确保一次完整采样不会与三轨TPS配置交叉执行。
 */
int monitor_task_prepare(void);

/** 持电源事务锁获取OFF维护占用；0成功，非0为忙/状态不允许，不自动下电。 */
int monitor_ota_enter(void);
/** 只取消未提交下载时释放；READY/提交不确定保持到复位。 */
void monitor_ota_leave(void);

/*
 * 串行执行0x0509业务：OFF状态只允许全零steps选择内存类型；ON状态只允许
 * 当前类型执行三路20mV拉偏。返回pwr_ctrl结果供调用方生成应答。
 */
pwr_ctrl_result_t monitor_apply_ddr_config(
    ddr_memory_type_t type, const int16_t steps[DDR_RAIL_COUNT]);

/* now_ms为调用时当前毫秒值；两个请求均补偿取锁耗时，不让锁等待吃掉轨间延时。 */
pwr_ctrl_result_t monitor_request_power_on(uint32_t now_ms);
pwr_ctrl_result_t monitor_request_power_off(uint32_t now_ms);

/**
 * @brief 在同一事务锁内保存训练目标并请求上电，防止目标选择和上电之间被插入操作。
 * @param[in] type LPDDR4或LPDDR4X。
 * @param[in] mv 非NULL、3项VDD1/VDD2/VDDQ目标mV；受理后不再借用缓冲。
 * @param[in] now_ms 与process同一时基的当前毫秒值；内部补偿取锁等待。
 * @return 锁忙为PWR_CTRL_ERR_RESOURCE_BUSY，其它结果见pwr_ctrl_request_training_on。
 * @note 任务上下文，取锁最多500ms；必须先安全下电到OFF、完成Boot和目标审核。
 *       OK只表示受理；不自动冷重启、不确认RK-ready、不证明真实电压安全。
 */
pwr_ctrl_result_t monitor_request_training_power_on(
    ddr_memory_type_t type, const uint16_t mv[DDR_RAIL_COUNT], uint32_t now_ms);

void monitor_power_process(uint32_t now_ms);

/**
 * @brief 在OFF状态持事务锁写Boot DAC，完整写成功才标记缓存有效。
 * @param[in] mode 有效boot_mode_t，目标电压0..1800mV。
 * @return OK写入成功；ERR_PARAM参数无效；ERR_BUSY锁忙；ERR_STATE状态不允许；
 *         ERR_IO器件写失败。实际电压和RK启动介质仍须独立确认。
 * @note 参数/忙/状态拒绝保留缓存；真正开始写入前使旧缓存失效，失败后
 *       查询返回ERR_NOT_SET。不写EEPROM，不新增上电/下电自动DAC动作。
 */
monitor_boot_result_t monitor_set_boot_mode(boot_mode_t mode);
/**
 * @brief 读取最近可信的Boot配置缓存，target_mv是目标mV，不是实测电压。
 * @param[out] mode 非NULL模式输出。
 * @param[out] target_mv 非NULL目标mV输出。
 * @return OK缓存有效；ERR_PARAM空指针；ERR_BUSY锁忙；ERR_NOT_SET未设置或
 *         最近写失败；ERR_IO目标换算失败。
 * @note 不回读DAC；未设置/锁忙时输出不变。MCU复位清空缓存；只重启RK
 *       不清缓存，因为DAC由3V3_MCU供电。缓存不等于RK启动成功确认。
 */
monitor_boot_result_t monitor_get_boot_mode(boot_mode_t *mode,
                                            uint16_t *target_mv);

void StartMonitorTask(void *argument);

#endif
