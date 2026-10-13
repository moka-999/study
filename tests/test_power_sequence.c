/* 执行真实pwr_ctrl及板级初值，只替代GPIO和DDR访问，不模拟真实电压或RK训练。 */
#include <stddef.h>
#include "main.h"
#include "board_power_config.h"

/* 位0/1/2分别记录PC6、PC7、PG11电平；不访问QEMU中的STM32寄存器。 */
static uint32_t s_levels, s_high_writes, s_ddr_calls;
static int s_ddr_result, s_training_result;

static void message(const char *text)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = text;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}

static _Noreturn void finish(uint32_t code)
{
    const uint32_t block[] = {0x20026U, code};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register const uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) {}
}

#define CHECK(condition) do { if (!(condition)) { \
    message("FAIL: " #condition "\n"); finish(1U); } } while (0)

void test_abort(void)
{
    message("FAIL: B1 power CPU exception\n");
    finish(1U);
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
    uint32_t bit = 0U;
    if (port == GPIOC && pin == GPIO_PIN_6) bit = 1U;
    if (port == GPIOC && pin == GPIO_PIN_7) bit = 2U;
    if (port == GPIOG && pin == GPIO_PIN_11) bit = 4U;
    CHECK(bit != 0U);
    if (state == GPIO_PIN_SET) {
        /* 可选PG11不能先于PC6/PC7两路使能释放，即使同一次process也检查调用顺序。 */
        if (bit == 4U) CHECK((s_levels & 3U) == 3U);
        s_levels |= bit;
        ++s_high_writes;
    } else {
        s_levels &= ~bit;
    }
}

int ddr_power_init(void)
{
    CHECK(s_levels == 7U);
    ++s_ddr_calls;
    return s_ddr_result;
}

int ddr_power_configure_training(ddr_memory_type_t type, const uint16_t mv[DDR_RAIL_COUNT])
{
    CHECK(s_levels == 0U && type == DDR_MEMORY_LPDDR4X && mv != NULL);
    return s_training_result;
}

/* 从全高假现场启动，验证init只关电，不因提供默认参数而偷偷启动或访问TPS。 */
static void reset_fixture(const pwr_ctrl_config_t *config)
{
    s_levels = 7U;
    s_high_writes = s_ddr_calls = 0U;
    s_ddr_result = s_training_result = IF_OK;
    pwr_ctrl_init(config);
    CHECK(s_levels == 0U && s_high_writes == 0U && s_ddr_calls == 0U);
    CHECK(pwr_ctrl_get_state() == PWR_CTRL_STATE_OFF);
}

/* 同时核对边沿之前/到期时刻；start靠近UINT32_MAX时覆盖毫秒计数回绕。 */
static void verify_default_cycle(uint32_t start)
{
    const uint16_t target[DDR_RAIL_COUNT] = {1800U, 1100U, 600U};
    reset_fixture(&b1_power_default_timing);
    CHECK(pwr_ctrl_is_configured());
    CHECK(pwr_ctrl_request_training_on(DDR_MEMORY_LPDDR4X, target, start) == PWR_CTRL_OK);
    CHECK(s_levels == 2U);
    pwr_ctrl_process(start + 9U);
    CHECK(s_levels == 2U && s_ddr_calls == 0U);
    pwr_ctrl_process(start + 10U);
    CHECK(s_levels == 7U && s_ddr_calls == 0U);
    pwr_ctrl_process(start + 14U);
    CHECK(s_ddr_calls == 0U);
    pwr_ctrl_process(start + 15U);
    CHECK(s_ddr_calls == 1U && pwr_ctrl_get_state() == PWR_CTRL_STATE_ON);
    pwr_ctrl_process(start + 16U);
    CHECK(s_ddr_calls == 1U);
    CHECK(pwr_ctrl_configure(&b1_power_default_timing) == PWR_CTRL_ERR_BUSY);
    CHECK(pwr_ctrl_request_off(start + 16U) == PWR_CTRL_OK);
    CHECK(s_levels == 2U);
    pwr_ctrl_process(start + 25U);
    CHECK(s_levels == 2U);
    pwr_ctrl_process(start + 26U);
    CHECK(s_levels == 0U && pwr_ctrl_get_state() == PWR_CTRL_STATE_OFF);
}

void test_main(void)
{
    reset_fixture(NULL);
    CHECK(!pwr_ctrl_is_configured());
    CHECK(pwr_ctrl_request_on(0U) == PWR_CTRL_ERR_NOT_CONFIGURED && s_levels == 0U);
    verify_default_cycle(100U);
    verify_default_cycle(UINT32_MAX - 4U);

    /* 第一阶段晚调度，不得把延误时间抵扣第二阶段的5ms等待。 */
    reset_fixture(&b1_power_default_timing);
    CHECK(pwr_ctrl_request_on(100U) == PWR_CTRL_OK);
    pwr_ctrl_process(120U);
    pwr_ctrl_process(124U);
    CHECK(s_levels == 7U && s_ddr_calls == 0U);
    s_ddr_result = IF_ERR;
    pwr_ctrl_process(125U);
    CHECK(s_ddr_calls == 1U && s_levels == 0U);
    CHECK(pwr_ctrl_get_state() == PWR_CTRL_STATE_FAULT);
    CHECK(pwr_ctrl_request_on(126U) == PWR_CTRL_ERR_FAULT);
    pwr_ctrl_process(1000U);
    CHECK(s_levels == 0U && s_ddr_calls == 1U);
    pwr_ctrl_clear_fault();
    CHECK(pwr_ctrl_get_state() == PWR_CTRL_STATE_OFF && pwr_ctrl_is_configured());

    /* 两个启动等待阶段都可立即取消；取消后不得访问TPS或延迟重新开电。 */
    for (uint32_t stage = 0U; stage < 2U; ++stage) {
        reset_fixture(&b1_power_default_timing);
        CHECK(pwr_ctrl_request_on(0U) == PWR_CTRL_OK);
        if (stage != 0U) pwr_ctrl_process(10U);
        CHECK(pwr_ctrl_request_off(11U) == PWR_CTRL_OK);
        pwr_ctrl_process(1000U);
        CHECK(s_levels == 0U && s_ddr_calls == 0U);
        CHECK(pwr_ctrl_get_state() == PWR_CTRL_STATE_OFF);
    }

    /* 训练目标校验失败应在第一轨动作前拒绝，而不是开电后补救。 */
    const uint16_t invalid_target[DDR_RAIL_COUNT] = {0U, 0U, 0U};
    reset_fixture(&b1_power_default_timing);
    s_training_result = IF_ERR;
    CHECK(pwr_ctrl_request_training_on(DDR_MEMORY_LPDDR4X, invalid_target, 0U) == PWR_CTRL_ERR_PARAM);
    CHECK(s_levels == 0U && s_high_writes == 0U);
    message("PASS: actual B1 power sequence, defaults/OFF/delays/reverse/fault/cancel/wrap/late tick\n");
    finish(0U);
}
