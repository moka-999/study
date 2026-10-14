/* 实际结果灯驱动的离线端口：记录六路GPIO和临界区，不访问任何板卡。 */
#include "result_led.h"
#include <stddef.h>

#define S1(x) #x
#define S(x) S1(x)
static uint32_t tick, levels, writes, critical, check_lock;
static GPIO_TypeDef port;

static void message(const char *text)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = text;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}
static _Noreturn void finish(uint32_t status)
{
    const uint32_t block[] = {0x20026U, status};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register const uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) { }
}
#define CHECK(c) do { if (!(c)) { message("FAIL LED line " S(__LINE__) "\n"); finish(1); } } while (0)
void test_abort(void) { message("FAIL LED CPU exception\n"); finish(1); }
uint32_t HAL_GetTick(void) { return tick; }
void mock_enter(void) { CHECK(!critical); critical = 1U; }
void mock_exit(void) { CHECK(critical == 1U); critical = 0U; }
void HAL_GPIO_WritePin(GPIO_TypeDef *gpio, uint16_t pin, GPIO_PinState state)
{
    CHECK(gpio == &port && pin && (pin & (pin - 1U)) == 0U && pin <= 32U);
    CHECK(!check_lock || critical == 1U);
    ++writes;
    if (state == GPIO_PIN_SET) levels |= pin;
    else levels &= ~pin;
}

void test_main(void)
{
    result_led_state_t states[] = {RESULT_LED_FAIL, RESULT_LED_PASS, RESULT_LED_FAIL};
    result_led_state_t out[RESULT_LED_TARGET_COUNT];
    result_led_board_map_t map;
    for (unsigned i = 0; i < RESULT_LED_TARGET_COUNT; ++i)
        map.pairs[i] = (result_led_pin_pair_t){&port, 1U << (2U * i), &port, 2U << (2U * i)};
    CHECK(result_led_init(NULL) == HAL_ERROR);
    CHECK(result_led_set_all(states) == HAL_ERROR && writes == 0U);
    CHECK(result_led_init(&map) == HAL_OK && levels == 0U && writes == 6U);
    check_lock = 1U;
    CHECK(result_led_set_all(states) == HAL_OK && !critical && writes == 12U && levels == 25U);
    CHECK(result_led_get_all(out) == HAL_OK && out[0] == RESULT_LED_FAIL && out[1] == RESULT_LED_PASS && out[2] == RESULT_LED_FAIL);
    states[0] = RESULT_LED_PASS; states[2] = (result_led_state_t)-1;
    CHECK(result_led_set_all(states) == HAL_ERROR && writes == 12U && levels == 25U);
    CHECK(result_led_set_all(NULL) == HAL_ERROR && writes == 12U);
    CHECK(result_led_get_all(out) == HAL_OK && out[0] == RESULT_LED_FAIL);

    states[0] = states[1] = RESULT_LED_TEST_NORMAL; states[2] = RESULT_LED_TEST_HOT;
    tick = 100U;
    CHECK(result_led_set_all(states) == HAL_OK && levels == 31U);
    result_led_process(599U); CHECK(levels == 31U);
    tick = 599U; CHECK(result_led_set_all(states) == HAL_OK);
    result_led_process(600U); CHECK(levels == 0U); /* 重复提交不延后500ms闪烁边界。 */
    result_led_process(2100U); CHECK(levels == 31U); /* 跨过3个半周期只更新一次。 */
    result_led_all_off(); CHECK(levels == 0U);
    tick = UINT32_MAX - 99U;
    CHECK(result_led_set_all(states) == HAL_OK && levels == 31U);
    result_led_process(399U); CHECK(levels == 31U);
    result_led_process(400U); CHECK(levels == 0U && !critical);
    message("PASS: actual LED C atomic batch, invalid input, idempotence, 500ms and tick wrap\n");
    finish(0U);
}
