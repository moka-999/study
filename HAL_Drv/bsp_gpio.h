/*
 * ===================================================================
 * 板级 GPIO 映射 — 阿波罗V2 验证 ↔ B1 正式
 * ===================================================================
 *
 * 设计:
 *   B1 板电源引脚语义固定。阿波罗V2底板P2/P3扩展排针恰好引出
 *   同一组GPIO，可直接验证最终引脚而不是使用替代脚。编译期宏用于
 *   区分板型并保护测试镜像:
 *     #define GPIO_USE_B1 1  → 用 B1 正式引脚 (B1 板到货后)
 *     #define GPIO_USE_B1 0  → 用 V2 测试引脚 (默认, 开发阶段)
 *
 * B1 正式引脚 (原理图 20260811 版确认):
 *   PWR_4V0_EN  PC6  → MP8759 EN (VCC4V0_SYS, 4.0V), 默认 OFF
 *   PWR_3V3_EN  PC7  → RT8070 EN (VCC3V3_SYS, 3.3V), 默认 OFF
 *   PWR_DDR_EN  PG11 → TPS EN防御性预留；0831图串联位NC，无实际总闸效果
 *   B1三轨随PC6产生4V自动启动；本文件的三段控制仅用于V2软件/GPIO验证，
 *   最终B1按独立工程的pwr_ctrl执行，不复制V2的独立PG11等待阶段。
 *
 *   (B1 MCU不负责加热；加热由B2 MCU统一控制)
 *
 * 阿波罗V2验证接点（官方底板原理图V2.6）:
 *   PWR_4V0_EN  PC6  → P2第31脚（DCMI_D0复用，未接摄像头时使用）
 *   PWR_3V3_EN  PC7  → P2第32脚（DCMI_D1复用，未接摄像头时使用）
 *   PWR_DDR_EN  PG11 → P3第31脚（LCD_B3复用，未接LCD时使用）
 *
 * PC6在现有CubeMX工程中仍有TIM8_CH1历史配置；pwr_ctrl_init()在
 * MX_TIM8_Init()之后调用，bsp_gpio_init()会将其重新配置为GPIO输出。
 * 后续单独清理CubeMX遗留时再删除TIM8，当前不手改生成代码。
 * ===================================================================
 */
#ifndef __BSP_GPIO_H
#define __BSP_GPIO_H

#include "main.h"

/* ── 编译期引脚表选择 ──
 * V2 阶段: 0 (默认)   B1 真板: 1
 */
#ifndef GPIO_USE_B1
#define GPIO_USE_B1  0
#endif

/* ── 功能枚举 (语义固定, 不随引脚表变化) ── */
typedef enum {
    GPIO_FUNC_PWR_4V0_EN = 0,  /* 4.0V 电源轨使能 (MP8759 EN)   */
    GPIO_FUNC_PWR_3V3_EN,      /* 3.3V 电源轨使能 (RT8070 EN)   */
    GPIO_FUNC_PWR_DDR_EN,      /* DDR 电源轨使能 (TPS62868 EN)  */
    GPIO_FUNC_COUNT,
} gpio_func_t;

/**
 * @brief 将电源使能脚预置为低，再配置成推挽输出。
 * @note GPIO时钟须已开启，且在既有TIM8初始化后调用；会接管PC6/PC7/PG11。
 *       由pwr_ctrl初始化调用，重复执行会立即关闭所有使能。
 */
void bsp_gpio_init(void);

/**
 * @brief 按板级功能映射设置GPIO电平。
 * @param[in] func 有效gpio_func_t；大于等于GPIO_FUNC_COUNT时忽略。
 * @param[in] level 0输出低，非0输出高。
 * @note 不执行电源时序/互锁，业务通过pwr_ctrl访问，避免绕过安全关断。
 */
void bsp_gpio_set(gpio_func_t func, uint8_t level);

/**
 * @brief 读取映射引脚的GPIO输入电平。
 * @param[in] func 有效gpio_func_t。
 * @return 1高；0低或功能编号超出范围。
 * @note GPIO电平不等于被控电源实际电压，不替代ADC回读。
 */
uint8_t bsp_gpio_get(gpio_func_t func);

#endif
