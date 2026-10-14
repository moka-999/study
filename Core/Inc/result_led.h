#ifndef RESULT_LED_H
#define RESULT_LED_H

/**
 * @file result_led.h
 * @brief 三颗双色测试结果灯的通用业务驱动
 *
 * 驱动只处理 PASS、FAIL、常温测试和高温测试的显示/闪烁语义，不把
 * 业务结果绑定到某个颜色名称。Apollo V2网络镜像使用红色模拟FAIL、绿色
 * 模拟PASS；B1再由自身板级表映射琥珀/蓝通道。双通道同步闪表示常温测试，
 * FAIL通道闪表示高温测试，物理对象顺序始终由调用方映射表提供。
 */

#include "stm32h7xx_hal.h"

#define RESULT_LED_TARGET_COUNT 3U
#define RESULT_LED_BLINK_MS   500U

typedef enum {
    RESULT_LED_LPDDR0 = 0,
    RESULT_LED_LPDDR1,
    RESULT_LED_EMCP,
} result_led_target_t;

typedef enum {
    RESULT_LED_OFF = 0,
    RESULT_LED_PASS,
    RESULT_LED_FAIL,
    RESULT_LED_TEST_NORMAL,
    RESULT_LED_TEST_HOT,
} result_led_state_t;

typedef struct {
    GPIO_TypeDef *fail_port;
    uint16_t fail_pin;
    GPIO_TypeDef *pass_port;
    uint16_t pass_pin;
} result_led_pin_pair_t;

typedef struct {
    result_led_pin_pair_t pairs[RESULT_LED_TARGET_COUNT];
} result_led_board_map_t;

/** 使用板级映射并立即关闭六路结果灯；GPIO须已配置，map=NULL返回HAL_ERROR。 */
HAL_StatusTypeDef result_led_init(const result_led_board_map_t *map);

/** 更新一组测试对象的显示状态；闪烁状态从亮相位开始。 */
HAL_StatusTypeDef result_led_set(result_led_target_t target, result_led_state_t state);

/** 原子提交三组状态；先完整校验，任何非法状态/未初始化均返回HAL_ERROR且不更新。
 * 数组依次LPDDR0/LPDDR1/eMCP，HAL_OK表示逻辑提交成功，不是灯珠电气回读。
 * 一次临界区内更新状态与GPIO，defaultTask不会观察到半份整轮汇总。
 */
HAL_StatusTypeDef result_led_set_all(const result_led_state_t states[RESULT_LED_TARGET_COUNT]);

/**
 * 一次复制三组业务状态，数组顺序为LPDDR0、LPDDR1、eMCP。
 * 返回的是业务状态，不是闪烁周期内某一时刻的GPIO亮灭电平。
 */
HAL_StatusTypeDef result_led_get_all(
    result_led_state_t states[RESULT_LED_TARGET_COUNT]);

/** 每次主循环调用；使用无符号时间差，可正确跨越HAL tick回绕。 */
void result_led_process(uint32_t now_ms);

/** 关闭三颗双色灯，不改变PG6单色状态灯。 */
void result_led_all_off(void);

#endif
