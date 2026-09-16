/*
 * ===================================================================
 * 心跳任务 — 独立于 rk_task
 * ===================================================================
 *
 * 每 1000ms 发送 Type=0x0000 心跳帧 (MCU→RK3588)
 * 通过rk_link_next_sequence()与rk_task共用RK序号，不暴露可写计数器。
 * ===================================================================
 */
#ifndef __HB_TASK_H
#define __HB_TASK_H

/**
 * @brief 周期发送RK心跳的FreeRTOS任务入口。
 * @param[in] argument 未使用，可为NULL。
 * @note UART须已注册并启动；每1000ms分配RK序号，发送失败等下一周期，
 *       不加入pending；任务持续运行，不正常返回。
 */
void StartHbTask(void *argument);

#endif
