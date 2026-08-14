/**
 ********************************************************************
 * @file    led.h
 * @brief   LED 状态指示灯抽象层
 * @note    PB0=心跳(绿), PB1=连接状态(红), 低电平亮
 ********************************************************************
 */
#ifndef __BSP_LED_H
#define __BSP_LED_H

#include "main.h"

/* ── LED 编号 ── */
#define LED_HEARTBEAT   0   /* PB0 — 心跳绿灯 (500ms 翻转) */
#define LED_CONNECT     1   /* PB1 — 连接红灯 (低有效: RESET=亮) */

/* ── 操作 ── */
void led_toggle(int led);       /* 翻转指定 LED          */
void led_on(int led);           /* 点亮 (低电平)         */
void led_off(int led);          /* 熄灭 (高电平)         */

#endif
