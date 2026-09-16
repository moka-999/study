/*
 * ===================================================================
 * LED 心跳任务 — 系统存活指示
 * ===================================================================
 * PB0 每 500ms 翻转一次, 绿灯闪烁 = MCU 正常运行
 * 阿波罗 V2: PB0=绿(心跳), PB1=红(连接), 低电平亮
 * ===================================================================
 */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "led.h"

/**
 * @brief FreeRTOS存活指示任务入口，周期翻转心跳灯。
 * @param[in] argument 未使用，可为NULL。
 * @note 调用前须初始化LED GPIO；每500ms翻转一次（完整明灭约1s）。
 *       仅表示任务得到调度，不证明网络/RK或其他业务健康，不正常返回。
 */
void StartLedBlinkTask(void *argument)
{
  (void)argument;
  for (;;) {
    led_toggle(LED_HEARTBEAT);               /* 翻转 PB0 — 心跳 */
    vTaskDelay(pdMS_TO_TICKS(500));          /* 500ms 周期 */
  }
}
