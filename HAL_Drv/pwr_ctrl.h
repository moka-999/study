/*
 * B1 系统电源状态机
 *
 * 管理两组真实硬件使能：PC6控制4.0V及DDR三轨，PC7控制3.3V系统电源。
 * 当前BRD的PG11到TPS EN路径为NC；固件仍按安全时序驱动它作为防御性
 * 预留总闸，但上下电结果和故障收敛始终以PC6/PC7为准。
 * 本模块只负责顺序和故障收敛，不决定尚未经过真板确认的具体毫秒值。
 * 单实例修改接口由同一控制任务串行调用；读到ON不代表已锁定电源，
 * 后续PC业务必须协调采样停止/安全掉电确认，不能由多任务并发推进。
 */
#ifndef PWR_CTRL_H
#define PWR_CTRL_H

#include <stdint.h>
#include "ddr_power.h"

/* PC6与PC7的先后由板级配置显式选择，联调初值仍须真板验收。 */
typedef enum {
    PWR_CTRL_RAIL_ORDER_4V0_FIRST = 0,
    PWR_CTRL_RAIL_ORDER_3V3_FIRST,
} pwr_ctrl_rail_order_t;

/*
 * 所有延时均为可标定参数，单位为毫秒。
 * V2 模拟值不能直接复制到 B1 量产配置；真板必须用示波器重新确认。
 */
typedef struct {
    pwr_ctrl_rail_order_t rail_order;
    uint32_t first_to_second_delay_ms;
    uint32_t second_to_tps_config_delay_ms;
    uint32_t system_rail_off_spacing_ms;
} pwr_ctrl_config_t;

typedef enum {
    PWR_CTRL_STATE_OFF = 0,
    PWR_CTRL_STATE_WAIT_FIRST_RAIL,
    PWR_CTRL_STATE_WAIT_SECOND_RAIL,
    PWR_CTRL_STATE_ON,
    PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF,
    PWR_CTRL_STATE_FAULT,
} pwr_ctrl_state_t;

typedef enum {
    PWR_CTRL_OK = 0,
    PWR_CTRL_ERR_PARAM = -1,
    PWR_CTRL_ERR_NOT_CONFIGURED = -2,
    PWR_CTRL_ERR_BUSY = -3,
    PWR_CTRL_ERR_FAULT = -4,
    PWR_CTRL_ERR_RESOURCE_BUSY = -5,
} pwr_ctrl_result_t;

/* 初始化 GPIO 并强制全部电源关闭；config=NULL 时禁止执行上电。 */
void pwr_ctrl_init(const pwr_ctrl_config_t *config);

/* 仅允许在 OFF 状态修改时序，防止运行中改变电源顺序。 */
pwr_ctrl_result_t pwr_ctrl_configure(const pwr_ctrl_config_t *config);

/* 请求非阻塞上电/下电；now_ms使用同一毫秒时基；上电沿用已选训练值，默认标称。 */
pwr_ctrl_result_t pwr_ctrl_request_on(uint32_t now_ms);
pwr_ctrl_result_t pwr_ctrl_request_off(uint32_t now_ms);

/**
 * @brief 在OFF时复制训练目标并请求上电，process在TPS可访问后写入目标。
 * @param[in] type LPDDR4或LPDDR4X。
 * @param[in] mv 非NULL、3项VDD1/VDD2/VDDQ目标mV；受理后不再借用缓冲。
 * @param[in] now_ms 与process共用的当前毫秒值。
 * @return OK受理；ERR_PARAM目标非法；ERR_NOT_CONFIGURED时序未配置；
 *         ERR_BUSY非OFF（含ON）；ERR_FAULT故障。拒绝请求不修改目标/电源。
 * @note 必须先审核目标安全范围、完成正常掉电/放电及Boot选择；本层只检查
 *       TPS编码，不编排自动冷重启或等待RK训练就绪；这些由上层业务负责。
 *       任务使用monitor_request_training_power_on串行调用；天然窗口待真板验收。
 */
pwr_ctrl_result_t pwr_ctrl_request_training_on(
    ddr_memory_type_t type, const uint16_t mv[DDR_RAIL_COUNT], uint32_t now_ms);

/*
 * 器件类型只能在OFF状态选择，并撤销自定义训练值、恢复标称目标；
 * 运行期拉偏只能在ON且调用方确认RK完成DDR初始化后执行，不改下一次训练值。
 * 任一轨调压失败会进入FAULT并立即关闭全部电源。
 */
pwr_ctrl_result_t pwr_ctrl_select_memory_type(ddr_memory_type_t type);
pwr_ctrl_result_t pwr_ctrl_apply_ddr_steps(const int16_t steps[DDR_RAIL_COUNT]);

/*
 * 周期调用以推进状态；各段毫秒等待不会休眠或占用 CPU。
 * 注意：进入 DDR 配置阶段时会调用现有的同步 I2C 驱动，因此该次调用可能
 * 被单笔总线超时短暂阻塞；后续若提高实时性要求，应把 DDR 配置拆成异步步骤。
 */
void pwr_ctrl_process(uint32_t now_ms);

/* FAULT 清除会再次强制全部关闭，之后回到 OFF。 */
void pwr_ctrl_clear_fault(void);

pwr_ctrl_state_t pwr_ctrl_get_state(void);
uint8_t pwr_ctrl_is_configured(void);

#endif
