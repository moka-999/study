#ifndef HANDOFF_RK_H
#define HANDOFF_RK_H

#include "frame.h"

/* 只读原RK任务的请求/结果占用；1表示可交接，不取走任何请求。 */
int rk_task_handoff_idle(void);
/* 仅RK任务调用，复用SPL频率查询、心跳和就绪窗口；不投递算法。 */
void rk_task_handoff_receive(const tlv_frame_t *frame);
void rk_task_handoff_heartbeat(void);

#endif
