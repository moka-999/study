/**
 ********************************************************************
 * @file    led.c
 * @brief   LED 指示灯 — GPIO 操作封装
 * @note    Apollo V2: PB0=绿(心跳), PB1=红(连接)
 *          最终板PG0-PG6高有效：3颗双色结果灯(2xLPDDR+1xeMCP)+LED4状态灯；
 *          CPU_GPIO_PWR另控LED5。电气模型/API不同，不能只改引脚宏；见需求基线。
 ********************************************************************
 */
#include "led.h"

/* ── 引脚查找表 ── */
static GPIO_TypeDef *const led_ports[] = { GPIOB, GPIOB };
static const uint16_t      led_pins[]  = { GPIO_PIN_0, GPIO_PIN_1 };

void led_toggle(int led)
{
  if (led < 0 || led > 1) return;
  HAL_GPIO_TogglePin(led_ports[led], led_pins[led]);
}

void led_on(int led)
{
  if (led < 0 || led > 1) return;
  HAL_GPIO_WritePin(led_ports[led], led_pins[led], GPIO_PIN_RESET);  /* 低有效 */
}

void led_off(int led)
{
  if (led < 0 || led > 1) return;
  HAL_GPIO_WritePin(led_ports[led], led_pins[led], GPIO_PIN_SET);
}
