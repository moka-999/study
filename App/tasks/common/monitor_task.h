/* 系统监测任务向PC–MCU JSON业务层提供快照及持事务锁的控制入口。 */
#ifndef MONITOR_TASK_H
#define MONITOR_TASK_H

#include "memory_monitor.h"
#include "idd_stats.h"
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

/**
 * @brief 复制最近发布的监测快照，不触发即时采样。
 * @param[out] snapshot 非NULL调用方缓冲；无快照时保持原值。
 * @return 1已复制；0尚无快照或参数为NULL。
 * @note 任务上下文调用，临界区仅保护复制。使用前须检查power_state、
 *       memory.valid_mask及timestamp_ms新鲜度；存在快照不等于测量有效。
 */
int monitor_get_latest(monitor_snapshot_t *snapshot);

/**
 * 任务上下文启动统计：IF_OK成功；IF_ERR参数无效；IF_TIMEOUT器件事务锁忙；
 * IF_ERR也表示电源非ON、OTA维护/未健康或已有窗口。拒绝时旧统计保持不变。
 * 参数范围同idd_stats_begin；仅收集后续实际新采样，不重复使用s_latest。
 * ponytail: 沿用约2秒后台采样；高频需求须先冻结RK/器件时窗再调整调度。
 * 不保证短窗口足够样本，不触发RK/温控动作。
 */
int monitor_idd_start(uint32_t duration_ms, uint32_t minimum_samples);
/** 任务上下文取消当前窗口，不停ADC、不清空部分统计、不改变电源。 */
void monitor_idd_cancel(void);
/** 冻结当前统计，采样足够为COMPLETE，否则TIMEOUT；不代表算法PASS/FAIL。 */
void monitor_idd_finish(void);
/** 复制并推进到期状态：1成功，0空指针；检查state及每项valid_count后使用。 */
int monitor_idd_get(idd_stats_t *out);

/**
 * @brief 调度前建立采样/电源/PC配置共用事务锁并绑定Boot DAC。
 * @return 1锁可用；0创建失败。
 * @note 重复调用复用静态锁；不访问硬件。须在启动相关任务前完成。
 */
int monitor_task_prepare(void);

/** OTA取得OFF状态的维护占用；与上电/调压使用同一事务锁，0表示占用成功。 */
int monitor_ota_enter(void);
/** 仅取消尚未提交的下载时释放维护占用；READY提交后一直保留到复位。 */
void monitor_ota_leave(void);

/**
 * @brief 持事务锁执行PC的内存类型选择或三轨拉偏。
 * @param[in] type LPDDR4或LPDDR4X；ON时必须等于当前类型。
 * @param[in] steps 非NULL、3项，顺序VDD1/VDD2/VDDQ，每档10/5/5mV；OFF时须全零。
 * @return PWR_CTRL_OK成功；ERR_PARAM非法目标；ERR_NOT_CONFIGURED锁未建立；
 *         ERR_RESOURCE_BUSY取锁失败；ERR_BUSY状态/类型不允许；ERR_FAULT故障。
 * @note 任务上下文，取锁最多等待500ms后还可能同步I2C阻塞。ON拉偏仍要求
 *       PC确保RK已完成DDR初始化；写失败由pwr_ctrl全关并进入FAULT。
 */
pwr_ctrl_result_t monitor_apply_ddr_config(
    ddr_memory_type_t type, const int16_t steps[DDR_RAIL_COUNT]);

/**
 * @brief 持事务锁请求上电。
 * @param[in] now_ms 调用时的当前毫秒值，与monitor_power_process同一时基；内部补偿取锁耗时。
 * @return 取锁失败返回PWR_CTRL_ERR_RESOURCE_BUSY；否则传递pwr_ctrl请求结果。
 * @note 仅任务上下文，最多等待锁500ms；OK表示受理或已在目标状态，
 *       不保证上电完成，后续须周期推进状态机。
 */
pwr_ctrl_result_t monitor_request_power_on(uint32_t now_ms);

/**
 * @brief 持同一事务锁保存本次训练目标并请求冷启动的上电阶段。
 * @param[in] type LPDDR4或LPDDR4X。
 * @param[in] mv 非NULL、3项VDD1/VDD2/VDDQ目标mV；参数在请求受理时复制。
 * @param[in] now_ms 调用时的当前毫秒值，与monitor_power_process同一时基；内部补偿取锁耗时。
 * @return 取锁失败为ERR_RESOURCE_BUSY；其它结果见pwr_ctrl_request_training_on。
 * @note 任务上下文，取锁最多500ms，锁取得前不会为本请求打开电源。
 *       必须已安全下电到OFF并完成Boot选择；不替代电源循环/RK就绪编排。
 *       OK只是受理，不代表电压稳定或训练完成；安全范围须由业务先行审核。
 */
pwr_ctrl_result_t monitor_request_training_power_on(
    ddr_memory_type_t type, const uint16_t mv[DDR_RAIL_COUNT], uint32_t now_ms);

/**
 * @brief 持事务锁请求下电。
 * @param[in] now_ms 调用时的当前毫秒值，与monitor_power_process同一时基；内部补偿取锁耗时。
 * @return 取锁失败返回PWR_CTRL_ERR_RESOURCE_BUSY；否则传递pwr_ctrl请求结果。
 * @note 仅任务上下文，最多等待锁500ms；OK表示受理或已在目标状态，
 *       不保证下电完成。正常下电前调用方必须取得RK安全掉电确认。
 */
pwr_ctrl_result_t monitor_request_power_off(uint32_t now_ms);

/**
 * @brief 尝试取得事务锁并推进一次电源状态机。
 * @param[in] now_ms 与电源请求共用的毫秒时基。
 * @note 锁忙时立即跳过，留待下次调用；取锁后DDR初始化可能同步阻塞。
 *       由defaultTask周期调用，不在ISR运行。
 */
void monitor_power_process(uint32_t now_ms);

/**
 * @brief 在电源OFF时持事务锁写Boot DAC并缓存成功配置。
 * @param[in] mode 有效boot_mode_t，目标范围0..1800mV。
 * @return MONITOR_BOOT_OK写入成功；ERR_PARAM模式无效；ERR_BUSY锁失败；
 *         ERR_STATE非OFF；ERR_IO写入失败。
 * @note 取锁最多等待500ms；成功只代表写入，非VOUT实测或RK启动确认。
 *       参数/忙/状态拒绝保留缓存；一旦开始写入先失效旧缓存，写失败后
 *       查询返回ERR_NOT_SET，不能用之前的成功模式冒充当前硬件状态。
 */
monitor_boot_result_t monitor_set_boot_mode(boot_mode_t mode);

/**
 * @brief 持锁读取最近一次成功写入的Boot模式及目标电压。
 * @param[out] mode 非NULL模式输出。
 * @param[out] target_mv 非NULL目标mV输出，不是实测电压。
 * @return MONITOR_BOOT_OK成功；ERR_PARAM空指针；ERR_BUSY取锁失败；
 *         ERR_NOT_SET尚未成功设置或最近写入失败；ERR_IO目标换算失败。
 * @note 任务上下文，取锁最多500ms；未设置/取锁失败时输出保持原值，
 *       换算失败时mode可能已写入。不从DAC寄存器回读，不证明实际启动介质。
 *       MCU复位后缓存清空；只重启RK不清缓存，因为DAC由独立3V3_MCU供电。
 */
monitor_boot_result_t monitor_get_boot_mode(boot_mode_t *mode,
                                            uint16_t *target_mv);

/**
 * @brief 监测任务入口，周期发布电源状态及三轨采样快照。
 * @param[in] argument 未使用，可为NULL。
 * @note prepare须在调度前完成；仅ON时初始化/采样，非ON清就绪标志。
 *       V2使用本工程映射；循环等待2000ms，另有总线/锁耗时，不正常返回。
 */
void StartMonitorTask(void *argument);

#endif
