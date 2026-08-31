/* 系统监测任务对未来PC–MCU JSON业务层提供的快照。 */
#ifndef MONITOR_TASK_H
#define MONITOR_TASK_H

#include "memory_monitor.h"
#include "pwr_ctrl.h"
#include <stdint.h>

typedef struct {
    uint32_t timestamp_ms;
    pwr_ctrl_state_t power_state;
    memory_monitor_sample_t memory;
} monitor_snapshot_t;

/*
 * 任务上下文复制最新快照：1=存在快照，0=尚无快照或输出指针为空。
 * 输出由调用方持有，不暴露任务内部缓冲；临界区只包围结构复制。
 * “有快照”不等于测量有效：必须检查power_state、memory.valid_mask和
 * timestamp_ms的新鲜度。不会触发即时硬件采样，也不等待下次采样。
 */
int monitor_get_latest(monitor_snapshot_t *snapshot);
void StartMonitorTask(void *argument);

#endif
