/*
 * B1 系统电源状态机
 *
 * 管理三组硬件使能：4.0V 系统输入、3.3V 系统电源和 DDR 三轨总使能。
 * 本模块只负责顺序和故障收敛，不决定尚未经过真板确认的具体毫秒值。
 */
#ifndef PWR_CTRL_H
#define PWR_CTRL_H

#include <stdint.h>
#include "ddr_power.h"

/* PC6 与 PC7 的先后关系必须由最终硬件评审结果显式选择。 */
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
    uint32_t second_to_ddr_delay_ms;
    uint32_t ddr_enable_to_config_delay_ms;
    uint32_t ddr_to_system_off_delay_ms;
    uint32_t system_rail_off_spacing_ms;
} pwr_ctrl_config_t;

typedef enum {
    PWR_CTRL_STATE_OFF = 0,
    PWR_CTRL_STATE_WAIT_FIRST_RAIL,
    PWR_CTRL_STATE_WAIT_SECOND_RAIL,
    PWR_CTRL_STATE_WAIT_DDR_READY,
    PWR_CTRL_STATE_ON,
    PWR_CTRL_STATE_WAIT_DDR_OFF,
    PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF,
    PWR_CTRL_STATE_FAULT,
} pwr_ctrl_state_t;

typedef enum {
    PWR_CTRL_OK = 0,
    PWR_CTRL_ERR_PARAM = -1,
    PWR_CTRL_ERR_NOT_CONFIGURED = -2,
    PWR_CTRL_ERR_BUSY = -3,
    PWR_CTRL_ERR_FAULT = -4,
} pwr_ctrl_result_t;

/* 初始化 GPIO 并强制全部电源关闭；config=NULL 时禁止执行上电。 */
void pwr_ctrl_init(const pwr_ctrl_config_t *config);

/* 仅允许在 OFF 状态修改时序，防止运行中改变电源顺序。 */
pwr_ctrl_result_t pwr_ctrl_configure(const pwr_ctrl_config_t *config);

/* 请求非阻塞上电/下电；now_ms 应使用同一单调递增毫秒时基。 */
pwr_ctrl_result_t pwr_ctrl_request_on(uint32_t now_ms);
pwr_ctrl_result_t pwr_ctrl_request_off(uint32_t now_ms);

/*
 * 器件类型只能在OFF状态选择；拉偏只能在完成标称电压初始化的ON状态执行。
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

