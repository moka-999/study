/*
 * RK任务之间共享的运行时资源，不是线上协议头。
 * 队列仍由Core初始化代码创建；这里只统一类型正确的声明。
 * 现有队列载荷属于历史TLV适配，正式PC JSON任务尚未接入。
 */
#ifndef RK_LINK_H
#define RK_LINK_H

#include "FreeRTOS.h"
#include "queue.h"

#include <stdint.h>

extern QueueHandle_t pcToRk_Queue;
extern QueueHandle_t rkToPc_Queue;

/*
 * 为一条新的RK命令或心跳分配u64序号。任务上下文调用，内部临界区
 * 保护64位读改写；不得返回给PC，也不用于重传（重传使用原序号）。
 * 固件启动后从0开始，允许发送失败留下间隙；不持久化到Flash。
 */
uint64_t rk_link_next_sequence(void);

#endif /* RK_LINK_H */
