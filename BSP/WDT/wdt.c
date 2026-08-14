/*
 * IWDG 看门狗 — 喂狗实现
 * 基于 CubeMX 生成的 MX_IWDG1_Init() (Prescaler=/32, Reload=4095, ~4s 超时)
 */
#include "wdt.h"
#include "iwdg.h"

/* 喂狗: 写 0xAAAA 到密钥寄存器, 重置递减计数器 */
void wdt_feed(void)
{
  HAL_IWDG_Refresh(&hiwdg1);
}
