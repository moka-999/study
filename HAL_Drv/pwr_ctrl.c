/*
 * B1 系统电源状态机实现
 *
 * 安全原则：
 * 1. 初始化和任何 DDR 配置失败都把 PC6/PC7/PG11 收敛到低电平。
 * 2. 未提供经过评审的时序配置时，上电请求直接拒绝。
 * 3. 上电过程中收到关机请求时立即全部关闭；只有稳定 ON 状态才执行
 *    “DDR -> 第二系统电源 -> 第一系统电源”的受控反向下电。
 * 4. 用 now_ms 差值判断延时，天然兼容 32 位毫秒计数器回绕。
 *
 * “非阻塞”仅指轨间等待不使用 HAL_Delay/vTaskDelay；DDR 寄存器配置沿用
 * 现有同步 I2C 驱动，单次调用仍受底层总线超时时间约束。
 */
#include "pwr_ctrl.h"

#include "bsp_gpio.h"
#include "ddr_power.h"

#if defined(ENABLE_PWR_WAVE_TEST) && ENABLE_PWR_WAVE_TEST && GPIO_USE_B1
#error "ENABLE_PWR_WAVE_TEST 只允许用于阿波罗 V2 验证，禁止用于 B1 真板"
#endif

static pwr_ctrl_config_t s_config;
static pwr_ctrl_state_t s_state = PWR_CTRL_STATE_OFF;
static uint32_t s_state_enter_ms;
static uint8_t s_configured;

static gpio_func_t first_system_rail(void)
{
    return s_config.rail_order == PWR_CTRL_RAIL_ORDER_4V0_FIRST
         ? GPIO_FUNC_PWR_4V0_EN : GPIO_FUNC_PWR_3V3_EN;
}

static gpio_func_t second_system_rail(void)
{
    return s_config.rail_order == PWR_CTRL_RAIL_ORDER_4V0_FIRST
         ? GPIO_FUNC_PWR_3V3_EN : GPIO_FUNC_PWR_4V0_EN;
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
    bsp_gpio_set(GPIO_FUNC_PWR_DDR_EN, 0);
    bsp_gpio_set(GPIO_FUNC_PWR_3V3_EN, 0);
    bsp_gpio_set(GPIO_FUNC_PWR_4V0_EN, 0);
}

#if !defined(ENABLE_PWR_WAVE_TEST) || !ENABLE_PWR_WAVE_TEST
static void enter_fault(void)
{
    force_all_rails_off();
    s_state = PWR_CTRL_STATE_FAULT;
}
#endif

pwr_ctrl_result_t pwr_ctrl_configure(const pwr_ctrl_config_t *config)
{
    if (!config || config->rail_order > PWR_CTRL_RAIL_ORDER_3V3_FIRST)
        return PWR_CTRL_ERR_PARAM;
    if (s_state != PWR_CTRL_STATE_OFF)
        return PWR_CTRL_ERR_BUSY;

    s_config = *config;
    s_configured = 1;
    return PWR_CTRL_OK;
}

void pwr_ctrl_init(const pwr_ctrl_config_t *config)
{
    /* bsp_gpio_init() 会先配置输出再写低；随后重复关断是有意的安全兜底。 */
    bsp_gpio_init();
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

    bsp_gpio_set(first_system_rail(), 1);
    enter_state(PWR_CTRL_STATE_WAIT_FIRST_RAIL, now_ms);
    return PWR_CTRL_OK;
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

    bsp_gpio_set(GPIO_FUNC_PWR_DDR_EN, 0);
    enter_state(PWR_CTRL_STATE_WAIT_DDR_OFF, now_ms);
    return PWR_CTRL_OK;
}

void pwr_ctrl_process(uint32_t now_ms)
{
    switch (s_state) {
    case PWR_CTRL_STATE_WAIT_FIRST_RAIL:
        if (delay_elapsed(now_ms, s_config.first_to_second_delay_ms)) {
            bsp_gpio_set(second_system_rail(), 1);
            enter_state(PWR_CTRL_STATE_WAIT_SECOND_RAIL, now_ms);
        }
        break;

    case PWR_CTRL_STATE_WAIT_SECOND_RAIL:
        if (delay_elapsed(now_ms, s_config.second_to_ddr_delay_ms)) {
            /* 三颗 TPS62868 的 VIN 来自 4.0V，因此 PG11 必须最后使能。 */
            bsp_gpio_set(GPIO_FUNC_PWR_DDR_EN, 1);
            enter_state(PWR_CTRL_STATE_WAIT_DDR_READY, now_ms);
        }
        break;

    case PWR_CTRL_STATE_WAIT_DDR_READY:
        if (delay_elapsed(now_ms, s_config.ddr_enable_to_config_delay_ms)) {
#if defined(ENABLE_PWR_WAVE_TEST) && ENABLE_PWR_WAVE_TEST
            /*
             * V2 未装三颗 TPS62868，波形镜像只验证同名GPIO状态迁移。
             * 此分支由独立构建选项启用，且上方编译期检查禁止其用于 B1。
             */
            enter_state(PWR_CTRL_STATE_ON, now_ms);
#else
            /* EN 拉高后寄存器才可访问；任一轨初始化失败都视为整机上电失败。 */
            if (ddr_power_init() != IF_OK) {
                enter_fault();
            } else {
                enter_state(PWR_CTRL_STATE_ON, now_ms);
            }
#endif
        }
        break;

    case PWR_CTRL_STATE_WAIT_DDR_OFF:
        if (delay_elapsed(now_ms, s_config.ddr_to_system_off_delay_ms)) {
            bsp_gpio_set(second_system_rail(), 0);
            enter_state(PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF, now_ms);
        }
        break;

    case PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF:
        if (delay_elapsed(now_ms, s_config.system_rail_off_spacing_ms)) {
            bsp_gpio_set(first_system_rail(), 0);
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
