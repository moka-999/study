/*
 * B1 系统电源状态机实现
 *
 * 当前BRD硬件合同：PG11到三颗TPS EN的串联电阻均为NC，TPS EN由
 * VCC4V0_SYS上拉。因此PC6始终是4V及三路DDR电源的可靠总开关。
 * 固件仍按安全时序驱动PG11作为防御性预留：当前板上无电气效果；若后续
 * 装上R4073/R4081/R4086，则可在两路系统电源启动后统一释放三颗TPS。
 * 电源状态和故障关断绝不能依赖该预留信号是否实际接通。
 *
 * 安全原则：
 * 1. 初始化和任何DDR配置失败都关闭PC6/PC7，并把PG11防御性拉低。
 * 2. 未提供经过评审的时序配置时，上电请求直接拒绝。
 * 3. 上电过程中收到关机请求时立即全部关闭；只有稳定 ON 状态才执行
 *    “第二系统电源 -> 第一系统电源”的受控反向下电。
 * 4. 用 now_ms 差值判断延时，天然兼容 32 位毫秒计数器回绕。
 *
 * “非阻塞”仅指轨间等待不使用 HAL_Delay/vTaskDelay；DDR 寄存器配置沿用
 * 现有同步 I2C 驱动，单次调用仍受底层总线超时时间约束。
 */
#include "pwr_ctrl.h"

#include "ddr_power.h"
#include "main.h"

/* 状态机只把PC6和PC7视为真实使能；PG11仅是可能无效的防御性总闸。 */
typedef enum {
    POWER_ENABLE_4V0 = 0,
    POWER_ENABLE_3V3,
} power_enable_t;

static pwr_ctrl_config_t s_config;
static pwr_ctrl_state_t s_state = PWR_CTRL_STATE_OFF;
static uint32_t s_state_enter_ms;
static uint8_t s_configured;

static power_enable_t first_system_rail(void)
{
    return s_config.rail_order == PWR_CTRL_RAIL_ORDER_4V0_FIRST
         ? POWER_ENABLE_4V0 : POWER_ENABLE_3V3;
}

static power_enable_t second_system_rail(void)
{
    return s_config.rail_order == PWR_CTRL_RAIL_ORDER_4V0_FIRST
         ? POWER_ENABLE_3V3 : POWER_ENABLE_4V0;
}

static void write_enable(power_enable_t enable, GPIO_PinState state)
{
    switch (enable) {
    case POWER_ENABLE_4V0:
        HAL_GPIO_WritePin(B1_PWR_4V0_EN_PORT, B1_PWR_4V0_EN_PIN, state);
        break;
    case POWER_ENABLE_3V3:
        HAL_GPIO_WritePin(B1_PWR_3V3_EN_PORT, B1_PWR_3V3_EN_PIN, state);
        break;
    default:
        break;
    }
}

/*
 * PG11可能因三颗0R未装而悬空于TPS侧，因此只负责“有连接时多一层保护”。
 * 调用方仍必须通过PC6切断VCC4V0_SYS，不能把本GPIO写成功当作TPS已关断。
 */
static void write_optional_tps_gate(GPIO_PinState state)
{
    HAL_GPIO_WritePin(B1_PWR_DDR_EN_PORT, B1_PWR_DDR_EN_PIN, state);
}

static void enter_state(pwr_ctrl_state_t state, uint32_t now_ms)
{
    s_state = state;
    s_state_enter_ms = now_ms;
}

static uint8_t delay_elapsed(uint32_t now_ms, uint32_t delay_ms)
{
    return (uint32_t)(now_ms - s_state_enter_ms) >= delay_ms;
}

/* 故障路径必须一次性关断全部使能，不能依赖后续任务仍能运行。 */
static void force_all_rails_off(void)
{
    write_optional_tps_gate(GPIO_PIN_RESET);
    write_enable(POWER_ENABLE_3V3, GPIO_PIN_RESET);
    write_enable(POWER_ENABLE_4V0, GPIO_PIN_RESET);
}

static void enter_fault(void)
{
    force_all_rails_off();
    s_state = PWR_CTRL_STATE_FAULT;
}

pwr_ctrl_result_t pwr_ctrl_configure(const pwr_ctrl_config_t *config)
{
    if (!config || (uint32_t)config->rail_order > PWR_CTRL_RAIL_ORDER_3V3_FIRST)
        return PWR_CTRL_ERR_PARAM;
    if (s_state != PWR_CTRL_STATE_OFF)
        return PWR_CTRL_ERR_BUSY;

    s_config = *config;
    s_configured = 1;
    return PWR_CTRL_OK;
}

void pwr_ctrl_init(const pwr_ctrl_config_t *config)
{
    /* MX_GPIO_Init()已关闭PC6/PC7/PG11；这里再执行一次状态机兜底。 */
    force_all_rails_off();
    s_state = PWR_CTRL_STATE_OFF;
    s_state_enter_ms = 0;
    s_configured = 0;

    if (config)
        (void)pwr_ctrl_configure(config);
}

pwr_ctrl_result_t pwr_ctrl_request_on(uint32_t now_ms)
{
    if (!s_configured)
        return PWR_CTRL_ERR_NOT_CONFIGURED;
    if (s_state == PWR_CTRL_STATE_FAULT)
        return PWR_CTRL_ERR_FAULT;
    if (s_state == PWR_CTRL_STATE_ON)
        return PWR_CTRL_OK;
    if (s_state != PWR_CTRL_STATE_OFF)
        return PWR_CTRL_ERR_BUSY;

    write_enable(first_system_rail(), GPIO_PIN_SET);
    enter_state(PWR_CTRL_STATE_WAIT_FIRST_RAIL, now_ms);
    return PWR_CTRL_OK;
}

pwr_ctrl_result_t pwr_ctrl_request_training_on(
    ddr_memory_type_t type, const uint16_t mv[DDR_RAIL_COUNT], uint32_t now_ms)
{
    if (!s_configured) return PWR_CTRL_ERR_NOT_CONFIGURED;
    if (s_state == PWR_CTRL_STATE_FAULT) return PWR_CTRL_ERR_FAULT;
    if (s_state != PWR_CTRL_STATE_OFF) return PWR_CTRL_ERR_BUSY;

    /*
     * 先完整校验/复制训练目标，再开第一轨。ON时即使目标相同也不走此入口，
     * 更不能用新目标覆盖运行中的电压；电源变化的冷重启编排由业务层负责。
     */
    if (ddr_power_configure_training(type, mv) != IF_OK) {
        return PWR_CTRL_ERR_PARAM;
    }
    return pwr_ctrl_request_on(now_ms);
}

pwr_ctrl_result_t pwr_ctrl_request_off(uint32_t now_ms)
{
    if (s_state == PWR_CTRL_STATE_OFF)
        return PWR_CTRL_OK;
    if (s_state == PWR_CTRL_STATE_FAULT) {
        force_all_rails_off();
        return PWR_CTRL_ERR_FAULT;
    }

    if (s_state != PWR_CTRL_STATE_ON) {
        /* 未完成上电时没有可依赖的稳定轨，立即关断比继续等待更安全。 */
        force_all_rails_off();
        enter_state(PWR_CTRL_STATE_OFF, now_ms);
        return PWR_CTRL_OK;
    }

    /* 有连接时先关TPS；无连接时无副作用，PC6仍会在系统轨流程中可靠关断。 */
    write_optional_tps_gate(GPIO_PIN_RESET);
    /* 从最后开启的系统轨开始反序下电。 */
    write_enable(second_system_rail(), GPIO_PIN_RESET);
    enter_state(PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF, now_ms);
    return PWR_CTRL_OK;
}

pwr_ctrl_result_t pwr_ctrl_select_memory_type(ddr_memory_type_t type)
{
    if (s_state != PWR_CTRL_STATE_OFF) return PWR_CTRL_ERR_BUSY;
    return ddr_power_select_memory_type(type) == IF_OK
         ? PWR_CTRL_OK : PWR_CTRL_ERR_PARAM;
}

pwr_ctrl_result_t pwr_ctrl_apply_ddr_steps(const int16_t steps[DDR_RAIL_COUNT])
{
    uint16_t targets[DDR_RAIL_COUNT];

    if (s_state == PWR_CTRL_STATE_FAULT) return PWR_CTRL_ERR_FAULT;
    if (s_state != PWR_CTRL_STATE_ON) return PWR_CTRL_ERR_BUSY;

    /*
     * 先用纯计算接口拒绝空指针、越界或非20mV档位。参数错误不能被误判为
     * TPS/I2C故障，更不能因此触发整机故障关断。
     */
    if (ddr_power_build_step_targets(ddr_power_get_memory_type(), steps,
                                     targets) != IF_OK) {
        return PWR_CTRL_ERR_PARAM;
    }

    if (ddr_power_set(DDR_RAIL_MASK_ALL, targets) != IF_OK) {
        enter_fault();
        return PWR_CTRL_ERR_FAULT;
    }
    return PWR_CTRL_OK;
}

void pwr_ctrl_process(uint32_t now_ms)
{
    switch (s_state) {
    case PWR_CTRL_STATE_WAIT_FIRST_RAIL:
        if (delay_elapsed(now_ms, s_config.first_to_second_delay_ms)) {
            write_enable(second_system_rail(), GPIO_PIN_SET);
            /*
             * 两路系统使能均已发出后才释放可选TPS总闸；随后沿用现有
             * second_to_tps_config_delay_ms等待TPS启动，再访问I2C。
             */
            write_optional_tps_gate(GPIO_PIN_SET);
            enter_state(PWR_CTRL_STATE_WAIT_SECOND_RAIL, now_ms);
        }
        break;

    case PWR_CTRL_STATE_WAIT_SECOND_RAIL:
        if (delay_elapsed(now_ms, s_config.second_to_tps_config_delay_ms)) {
            /*
             * PC6产生4V后TPS和RK806并行启动。这里直接写保存的训练目标，
             * 不先写标称。开发暂按能赶在RK训练前完成；实际可访问等待、
             * 总线/锁/调度耗时和模拟稳定时间仍须标定，不能照搬V2展示延时。
             * ON只表示写入完成，不等于DDR-ready；首个事务失败立即全关。
             */
            if (ddr_power_init() != IF_OK) {
                enter_fault();
            } else {
                enter_state(PWR_CTRL_STATE_ON, now_ms);
            }
        }
        break;

    case PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF:
        if (delay_elapsed(now_ms, s_config.system_rail_off_spacing_ms)) {
            write_enable(first_system_rail(), GPIO_PIN_RESET);
            enter_state(PWR_CTRL_STATE_OFF, now_ms);
        }
        break;

    case PWR_CTRL_STATE_OFF:
    case PWR_CTRL_STATE_ON:
    case PWR_CTRL_STATE_FAULT:
    default:
        break;
    }
}

void pwr_ctrl_clear_fault(void)
{
    /* 函数名只表示“清故障”，不得被误用成运行状态下的紧急关机接口。 */
    if (s_state != PWR_CTRL_STATE_FAULT)
        return;

    force_all_rails_off();
    s_state = PWR_CTRL_STATE_OFF;
    s_state_enter_ms = 0;
}

pwr_ctrl_state_t pwr_ctrl_get_state(void)
{
    return s_state;
}

uint8_t pwr_ctrl_is_configured(void)
{
    return s_configured;
}
