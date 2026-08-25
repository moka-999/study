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

void StartLedBlinkTask(void *argument)
{
  (void)argument;
  for (;;) {
    led_toggle(LED_HEARTBEAT);               /* 翻转 PB0 — 心跳 */
    vTaskDelay(pdMS_TO_TICKS(500));          /* 500ms 周期 */
  }
}
