#include "boot_health.h"
#include <string.h>

void boot_health_init(boot_health_t *h, uint32_t expected_mask, uint32_t now)
{
    memset(h, 0, sizeof(*h)); h->expected_mask = expected_mask; h->start_ms = now;
}

void boot_health_progress(boot_health_t *h, uint32_t task_id, uint32_t now)
{
    if (task_id >= BOOT_HEALTH_TASKS) return;
    h->progress_ms[task_id] = now; h->seen_mask |= 1U << task_id;
}

int boot_health_poll(boot_health_t *h, uint32_t now)
{
    if (h->expected_mask == 0U || (h->expected_mask >> BOOT_HEALTH_TASKS) != 0U) return 0;
    int all = (h->seen_mask & h->expected_mask) == h->expected_mask;
    for (uint32_t i = 0U; i < BOOT_HEALTH_TASKS; ++i) {
        if ((h->expected_mask & (1U << i)) != 0U && (uint32_t)(now - h->progress_ms[i]) > 5000U) all = 0;
    }
    if (!all) {
        /* 只在首次全部就绪前允许25秒宽限，覆盖现有lwIP初始化上限；观察后卡死立即失效。 */
        return h->observing == 0U && (uint32_t)(now - h->start_ms) < 25000U ? 1 : 0;
    }
    if (h->observing == 0U) { h->observing = 1U; h->healthy_since = now; }
    return (uint32_t)(now - h->healthy_since) >= 30000U ? 2 : 1;
}
