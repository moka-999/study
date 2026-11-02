/* 在QEMU中运行独立看门狗逻辑，验证宽限、卡死锁存及毫秒回绕；不访问IWDG。 */
#include "task_watchdog.h"
#include <stdint.h>

static uint32_t now;
uint32_t HAL_GetTick(void) { return now; }

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
    for (;;) { }
}
void test_abort(void) { message("FAIL: watchdog exception\n"); finish(1U); }
#define CHECK(x) do { if (!(x)) { message("FAIL: " #x "\n"); finish(1U); } } while (0)
static void all_progress(void)
{
    for (uint32_t i = 0U; i < TASK_WATCHDOG_COUNT; ++i) task_watchdog_progress(i);
}
void test_main(void)
{
    task_watchdog_prepare(TASK_WATCHDOG_ALL);
    CHECK(task_watchdog_can_feed());
    now = 24999U;
    CHECK(task_watchdog_can_feed());
    now = 25000U;
    CHECK(!task_watchdog_can_feed());
    all_progress();
    CHECK(!task_watchdog_can_feed()); /* 迟到恢复不能撤销已经锁存的卡死。 */

    now = UINT32_MAX - 2000U;
    task_watchdog_prepare(TASK_WATCHDOG_ALL);
    all_progress();
    CHECK(task_watchdog_can_feed()); /* 不需要连续30秒版本确认。 */
    now += 5000U;
    CHECK(task_watchdog_can_feed());
    ++now;
    for (uint32_t i = 0U; i < TASK_WATCHDOG_RK; ++i) task_watchdog_progress(i);
    task_watchdog_progress(TASK_WATCHDOG_COUNT);
    CHECK(!task_watchdog_can_feed());

    task_watchdog_prepare(0U); /* 不含正式任务的隔离镜像，不误判为卡死。 */
    now += 100000U;
    CHECK(task_watchdog_can_feed());
    task_watchdog_prepare(1UL << TASK_WATCHDOG_COUNT);
    CHECK(!task_watchdog_can_feed());
    message("PASS: task watchdog grace / stale task latch / tick wrap / isolated mode\n");
    finish(0U);
}
