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

/* 返回1表示已产生过快照，0表示任务尚未完成首次采集。 */
int monitor_get_latest(monitor_snapshot_t *snapshot);
void StartMonitorTask(void *argument);

#endif
