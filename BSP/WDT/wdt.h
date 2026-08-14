/*
 * IWDG 看门狗 — 封装 CubeMX HAL
 *
 * 用法:
 *   wdt_init()  — MX_IWDG1_Init() 已完成, 此函数为预留接口
 *   wdt_feed()  — HAL_IWDG_Refresh(&hiwdg1)
 *
 * 超时: LSI 32KHz / 32 = 1KHz, Reload=4095 → ~4.1 秒
 */
#ifndef __WDT_H
#define __WDT_H

#include <stdint.h>

void wdt_feed(void);

#endif
