/*
 * ===================================================================
 * 板级 GPIO 映射 — 阿波罗V2 测试 ↔ B2 正式 双引脚表
 * ===================================================================
 *
 * 设计:
 *   B2 板引脚语义固定 (pwr_ctrl / 加热), 阿波罗 V2 开发阶段
 *   用空闲 GPIO 模拟同样的逻辑时序, 编译期宏切换引脚表:
 *     #define GPIO_USE_B2 1  → 用 B2 正式引脚 (B2 板到货后)
 *     #define GPIO_USE_B2 0  → 用 V2 测试引脚 (默认, 开发阶段)
 *
 * B2 正式引脚 (原理图 20260811 版几何法确认):
 *   PWR_4V0_EN  PC6  → MP8759 EN (VCC4V0_SYS, 4.0V), 默认 OFF
 *   PWR_3V3_EN  PC7  → RT8070 EN (VCC3V3_SYS, 3.3V), 默认 OFF
 *   PWR_DDR_EN  PG11 → TPS62868×3 EN (VDD1/VDD2/VDDQ), 默认 OFF
 *
 *   (加热 PH8/9/10 已移除: B1 MCU 不负责加热, 由 PC 操控独立加热模块)
 *
 * V2 测试引脚 (.ioc 已配为 GPIO_Output 或未占用):
 *   PWR_4V0_EN  PF2   (PC6 在 V2 上是 S_TIM8_CH1, 被占)
 *   PWR_3V3_EN  PC7   (V2 上本就是 GPIO_Output, 直接复用)
 *   PWR_DDR_EN  PG1   (V2 上空闲 GPIO_Output)
 * ===================================================================
 */
#ifndef __BSP_GPIO_H
#define __BSP_GPIO_H

#include "main.h"

/* ── 编译期引脚表选择 ──
 * V2 阶段: 0 (默认)   B2 真板: 1
 */
#ifndef GPIO_USE_B2
#define GPIO_USE_B2  0
#endif

/* ── 功能枚举 (语义固定, 不随引脚表变化) ── */
typedef enum {
    GPIO_FUNC_PWR_4V0_EN = 0,  /* 4.0V 电源轨使能 (MP8759 EN)   */
    GPIO_FUNC_PWR_3V3_EN,      /* 3.3V 电源轨使能 (RT8070 EN)   */
    GPIO_FUNC_PWR_DDR_EN,      /* DDR 电源轨使能 (TPS62868 EN)  */
    GPIO_FUNC_COUNT,
} gpio_func_t;

/* ── 初始化: 全部配置为推挽输出, 初始低电平 (默认 OFF) ── */
void bsp_gpio_init(void);

/* ── 电平控制: level 0=低 1=高 ── */
void bsp_gpio_set(gpio_func_t func, uint8_t level);

/* ── 电平读取 ── */
uint8_t bsp_gpio_get(gpio_func_t func);

#endif
