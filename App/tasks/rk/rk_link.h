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

/**
 * @brief 为新的RK命令或心跳分配独立u64序号。
 * @return 本次序号；固件启动从0递增，无符号回绕，不持久化。
 * @note 任务上下文调用，临界区保护64位读改写。发送失败允许序号间隙；
 *       重传使用原序号，不再分配；此序号不得写入PC响应头。
 */
uint64_t rk_link_next_sequence(void);

#endif /* RK_LINK_H */
