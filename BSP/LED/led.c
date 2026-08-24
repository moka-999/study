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

led_result_levels_t led_result_render(led_result_state_t state,
                                      uint8_t blink_on)
{
  led_result_levels_t levels = { 0U, 0U };

  switch (state) {
  case LED_RESULT_PASS:
    levels.green_on = 1U;
    break;

  case LED_RESULT_FAIL:
    levels.red_on = 1U;
    break;

  case LED_RESULT_TEST_NORMAL:
    /* 红绿同时点亮形成橙色；灭相位必须同时关闭。 */
    levels.red_on = blink_on ? 1U : 0U;
    levels.green_on = levels.red_on;
    break;

  case LED_RESULT_TEST_HOT:
    levels.red_on = blink_on ? 1U : 0U;
    break;

  case LED_RESULT_OFF:
  default:
    break;
  }

  return levels;
}
