/* 使用B1实际中断文件和HAL计时函数；只模拟TIM1回调到达，不验证实物定时精度。 */
#include "stm32h7xx_hal.h"

static TIM_HandleTypeDef s_timer = { .Instance = TIM1 };
static uint32_t s_tick_polls;
static uint8_t s_drive_tick;

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

static void check(int valid, const char *reason)
{
    if (!valid) {
        message(reason);
        finish(1U);
    }
}

void test_abort(void)
{
    message("FAIL: CPU exception\n");
    finish(1U);
}

/* 用读取时触发回调代替硬件IRQ，使HAL_Delay的等待可在QEMU中有界验证。 */
uint32_t HAL_GetTick(void)
{
    if (s_drive_tick) {
        check(++s_tick_polls < 1000U, "FAIL: HAL_Delay cannot advance\n");
        HAL_TIM_PeriodElapsedCallback(&s_timer);
    }
    return uwTick;
}

void test_main(void)
{
    uwTick = 0U;
    uwTickFreq = HAL_TICK_FREQ_1KHZ;
    HAL_TIM_PeriodElapsedCallback(&s_timer);
    check(uwTick == 1U, "FAIL: TIM1 callback does not increment HAL tick\n");

    TIM_HandleTypeDef other_timer = { .Instance = TIM2 };
    HAL_TIM_PeriodElapsedCallback(&other_timer);
    check(uwTick == 1U, "FAIL: another timer changed HAL tick\n");

    uwTick = UINT32_MAX;
    HAL_TIM_PeriodElapsedCallback(&s_timer);
    check(uwTick == 0U, "FAIL: HAL tick wrap\n");

    /* PHY启动的10ms和100ms延时，以及计数回绕时的同类等待。 */
    const uint32_t delays[] = {10U, 100U, 10U};
    for (unsigned i = 0U; i < 3U; ++i) {
        uwTick = i == 2U ? UINT32_MAX - 5U : 0U;
        uint32_t started = uwTick;
        s_tick_polls = 0U;
        s_drive_tick = 1U;
        HAL_Delay(delays[i]);
        s_drive_tick = 0U;
        check((uint32_t)(uwTick - started) >= delays[i] &&
              (uint32_t)(uwTick - started) <= delays[i] + 3U,
              "FAIL: HAL delay elapsed time\n");
    }
    message("PASS: B1 TIM1 tick, other timer isolation, wrap and PHY delays\n");
    finish(0U);
}
