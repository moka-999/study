/*
 * ===================================================================
 * PWM 接口 — STM32 HAL TIM 实现
 * ===================================================================
 * TIM8_CH1, PC6, 1KHz, 分辨率 1000 步 → 0~100% 占空比
 * CubeMX 配置: Prescaler=200-1, Period=1000-1
 * ===================================================================
 */
#include "hal_if.h"
#include "tim.h"

static void _set(pwm_if_t *p, uint8_t pc)
{
  TIM_HandleTypeDef *t = p->handle;
  if (pc > 100) pc = 100;
  /* 占空比 = (Period+1) * percent / 100 */
  __HAL_TIM_SET_COMPARE(t, TIM_CHANNEL_1, (uint32_t)(t->Init.Period + 1) * pc / 100);
}

/* ── 工厂实例 ── */
pwm_if_t fan_if = { .handle=&htim8, .set_duty=_set };
