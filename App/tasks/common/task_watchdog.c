#include "task_watchdog.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include <string.h>

/* 启动宽限覆盖lwIP初始化；开始正常监督后，任一任务超过5s不推进即停止喂狗。
 * 仅监督任务调度，不等待版本确认、不访问Flash，也不阻止业务命令。
 */
#define TASK_START_GRACE_MS 25000U
#define TASK_STALE_MS        5000U

static struct {
    uint32_t expected_mask;
    uint32_t seen_mask;
    uint32_t start_ms;
    uint32_t progress_ms[TASK_WATCHDOG_COUNT];
    uint8_t observing;
    uint8_t failed;
} s_watchdog;

void task_watchdog_prepare(uint32_t expected_mask)
{
    memset(&s_watchdog, 0, sizeof(s_watchdog));
    s_watchdog.expected_mask = expected_mask;
    s_watchdog.start_ms = HAL_GetTick();
    s_watchdog.failed = (expected_mask & ~TASK_WATCHDOG_ALL) != 0U;
}

void task_watchdog_progress(uint32_t task_id)
{
    if (task_id >= TASK_WATCHDOG_COUNT) return;
    taskENTER_CRITICAL();
    s_watchdog.progress_ms[task_id] = HAL_GetTick();
    s_watchdog.seen_mask |= 1UL << task_id;
    taskEXIT_CRITICAL();
}

int task_watchdog_can_feed(void)
{
    taskENTER_CRITICAL();
    uint32_t now = HAL_GetTick();
    int ready = (s_watchdog.seen_mask & s_watchdog.expected_mask) == s_watchdog.expected_mask;
    for (uint32_t i = 0U; i < TASK_WATCHDOG_COUNT; ++i) {
        if ((s_watchdog.expected_mask & (1UL << i)) != 0U &&
            (uint32_t)(now - s_watchdog.progress_ms[i]) > TASK_STALE_MS) ready = 0;
    }
    if (ready) s_watchdog.observing = 1U;
    else if (s_watchdog.observing ||
             (uint32_t)(now - s_watchdog.start_ms) >= TASK_START_GRACE_MS)
        s_watchdog.failed = 1U;
    int allowed = !s_watchdog.failed;
    taskEXIT_CRITICAL();
    return allowed;
}
