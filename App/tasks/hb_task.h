/*
 * ===================================================================
 * 心跳任务 — 独立于 rk_task
 * ===================================================================
 *
 * 每 1000ms 发送 Type=0x0000 心跳帧 (MCU→RK3588)
 * 与 rk_task 共享 g_rk_seq_counter (taskENTER_CRITICAL 保护)
 * ===================================================================
 */
#ifndef __HB_TASK_H
#define __HB_TASK_H

#include <stdint.h>

/* MCU→RK3588 统一序列号 (心跳 + 命令帧共用) */
extern volatile uint64_t g_rk_seq_counter;

void StartHbTask(void *argument);

#endif
