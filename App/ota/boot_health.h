#ifndef BOOT_HEALTH_H
#define BOOT_HEALTH_H
#include <stdint.h>

#define BOOT_HEALTH_TASKS 5U
/** 各必要任务的进展时间，不要求PC/RK/B2实际在线。单位ms，允许uint32自然回绕。 */
typedef struct {
    uint32_t expected_mask, seen_mask, start_ms, healthy_since, observing;
    uint32_t progress_ms[BOOT_HEALTH_TASKS];
} boot_health_t;

void boot_health_init(boot_health_t *health, uint32_t expected_mask, uint32_t now);
/** 由任务完成一轮真实工作/有界等待后调用；多任务调用须由App短临界区保护。 */
void boot_health_progress(boot_health_t *health, uint32_t task_id, uint32_t now);
/** 0失去健康，1初始化宽限/观察中可喂狗，2连续健康30秒可确认。 */
int boot_health_poll(boot_health_t *health, uint32_t now);
#endif
