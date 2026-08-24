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

/*
 * 三颗双色结果灯共用同一套业务语义。
 * 这里只描述“应点亮哪一种颜色”，不绑定 Apollo V2 或 B1 的物理引脚。
 */
typedef enum {
    LED_RESULT_OFF = 0,          /* 无结果或未开始：红、绿均灭 */
    LED_RESULT_PASS,             /* 测试通过：绿灯常亮 */
    LED_RESULT_FAIL,             /* 测试失败：红灯常亮 */
    LED_RESULT_TEST_NORMAL,      /* 常温测试中：橙色闪烁 */
    LED_RESULT_TEST_HOT,         /* 高温测试中：红色闪烁 */
} led_result_state_t;

typedef struct {
    uint8_t red_on;
    uint8_t green_on;
} led_result_levels_t;

/* ── LED 编号 ── */
#define LED_HEARTBEAT   0   /* PB0 — 心跳绿灯 (500ms 翻转) */
#define LED_CONNECT     1   /* PB1 — 连接红灯 (低有效: RESET=亮) */

/* ── 操作 ── */
void led_toggle(int led);       /* 翻转指定 LED          */
void led_on(int led);           /* 点亮 (低电平)         */
void led_off(int led);          /* 熄灭 (高电平)         */

/*
 * 把业务状态转换为红、绿两路逻辑电平。
 * blink_on=1 表示当前处于500ms闪烁的亮相位，0表示灭相位。
 */
led_result_levels_t led_result_render(led_result_state_t state,
                                      uint8_t blink_on);

#endif
