/* 器件最短等待：复用现有TIM1的1ms HAL时基，不新增定时器/中断。 */
#ifndef DEVICE_WAIT_H
#define DEVICE_WAIT_H

#include "main.h"
#include "FreeRTOS.h"
#include "task.h"

/**
 * @brief 实际等待至少milliseconds毫秒，同时让出CPU。
 * @param[in] milliseconds 0直接返回；大值分段，避免加1及时间回绕的边界溢出。
 * @note 仅调度器运行后的任务上下文，TIM1/HAL的1ms节拍必须运行且中断开启。
 *       每段要求HAL时基严格超过请求值，补偿进入函数时不足一个tick的相位。
 *       允许被抢占后更晚返回，不承诺精确唤醒；禁止ISR/关中断/挂起调度器调用。
 */
static inline void device_wait_ms(uint32_t milliseconds)
{
    while (milliseconds != 0U) {
        uint32_t part = milliseconds > 1000U ? 1000U : milliseconds;
        uint32_t begin = HAL_GetTick();
        do {
            vTaskDelay(1U);
        } while ((uint32_t)(HAL_GetTick() - begin) <= part);
        milliseconds -= part;
    }
}
#endif
