#ifndef TASK_WATCHDOG_H
#define TASK_WATCHDOG_H

#include <stdint.h>

/** 必要任务每完成一轮工作后上报存活；与PC/RK设备是否在线无关。 */
enum {
    TASK_WATCHDOG_DEFAULT = 0,
    TASK_WATCHDOG_TCP,
    TASK_WATCHDOG_MONITOR,
    TASK_WATCHDOG_RK,
    TASK_WATCHDOG_COUNT
};
#define TASK_WATCHDOG_ALL ((1UL << TASK_WATCHDOG_COUNT) - 1UL)

/** 调度前调用一次。正常镜像传ALL；隔离验证只传实际创建的任务位，0为不监督。 */
void task_watchdog_prepare(uint32_t expected_mask);
/** 完成一轮有界工作/等待后调用；task_id为上述枚举，无效编号忽略。 */
void task_watchdog_progress(uint32_t task_id);
/** 仅喂狗任务每500ms调用；1允许喂狗、0停止，失败锁存至MCU复位。 */
int task_watchdog_can_feed(void);

#endif
