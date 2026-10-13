/**
 * @file result_led.c
 * @brief B1 三组双色结果灯的业务状态机
 *
 * 原理图中PG0-PG5经1k电阻驱动2N7002栅极，MOS管导通后把LED阴极拉低，
 * 因此GPIO高电平表示点亮。闪烁采用500ms全局相位，三颗灯同时进入测试
 * 状态时会保持同步；本文件不使用HAL_Delay，不阻塞后续网关任务。
 * 业务层只使用FAIL/PASS通道名，实际颜色由真板确认，避免更换灯料号时误解。
 */

#include "result_led.h"
#include "FreeRTOS.h"
#include "task.h"

typedef struct {
    uint8_t fail_on;
    uint8_t pass_on;
} result_led_levels_t;

static const result_led_board_map_t *s_board_map;
static result_led_state_t s_states[RESULT_LED_TARGET_COUNT];
static uint32_t s_last_blink_ms;
static uint8_t s_blink_on;

static uint8_t result_led_is_blinking(result_led_state_t state)
{
    return (state == RESULT_LED_TEST_NORMAL || state == RESULT_LED_TEST_HOT) ? 1U : 0U;
}

static result_led_levels_t result_led_render(result_led_state_t state,
                                              uint8_t blink_on)
{
    result_led_levels_t levels = { 0U, 0U };

    switch (state) {
    case RESULT_LED_PASS:
        levels.pass_on = 1U;
        break;

    case RESULT_LED_FAIL:
        levels.fail_on = 1U;
        break;

    case RESULT_LED_TEST_NORMAL:
        /* 常温测试让两个颜色同步闪烁，与PASS/FAIL及高温警示明确区分。 */
        levels.fail_on = blink_on ? 1U : 0U;
        levels.pass_on = levels.fail_on;
        break;

    case RESULT_LED_TEST_HOT:
        /* FAIL通道单独闪烁，区分高温警示和常温双色闪烁。 */
        levels.fail_on = blink_on ? 1U : 0U;
        break;

    case RESULT_LED_OFF:
    default:
        break;
    }

    return levels;
}

static void result_led_apply(result_led_target_t target)
{
    const result_led_pin_pair_t *pair = &s_board_map->pairs[target];
    result_led_levels_t levels = result_led_render(s_states[target], s_blink_on);

    HAL_GPIO_WritePin(pair->fail_port, pair->fail_pin,
                      levels.fail_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(pair->pass_port, pair->pass_pin,
                      levels.pass_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void result_led_apply_all(void)
{
    for (uint32_t i = 0; i < RESULT_LED_TARGET_COUNT; ++i)
        result_led_apply((result_led_target_t)i);
}

HAL_StatusTypeDef result_led_init(const result_led_board_map_t *map)
{
    if (map == NULL)
        return HAL_ERROR;

    s_board_map = map;
    s_blink_on = 0U;
    s_last_blink_ms = HAL_GetTick();

    for (uint32_t i = 0; i < RESULT_LED_TARGET_COUNT; ++i)
        s_states[i] = RESULT_LED_OFF;

    result_led_apply_all();
    return HAL_OK;
}

HAL_StatusTypeDef result_led_set(result_led_target_t target, result_led_state_t state)
{
    if (s_board_map == NULL || (uint32_t)target >= RESULT_LED_TARGET_COUNT ||
        (uint32_t)state > RESULT_LED_TEST_HOT)
        return HAL_ERROR;

    /*
     * TCP任务会设置状态，defaultTask每1ms推进闪烁。临界区同时保护状态、
     * 全局闪烁相位和GPIO更新，避免查询或刷新读到一半更新的数据。区间内
     * 最多写六次GPIO BSRR，不执行延时或外设通信。
    */
    taskENTER_CRITICAL();
    if (s_states[target] == state) {
        /* TCP重试必须幂等，不能反复把全局500ms闪烁相位重置为亮。 */
        taskEXIT_CRITICAL();
        return HAL_OK;
    }
    s_states[target] = state;

    /* 新进入闪烁状态时立即点亮，避免最多500ms的首次显示延迟。 */
    if (result_led_is_blinking(state)) {
        s_blink_on = 1U;
        s_last_blink_ms = HAL_GetTick();
        result_led_apply_all();
    } else {
        result_led_apply(target);
    }
    taskEXIT_CRITICAL();

    return HAL_OK;
}

HAL_StatusTypeDef result_led_get_all(
    result_led_state_t states[RESULT_LED_TARGET_COUNT])
{
    if (s_board_map == NULL || states == NULL)
        return HAL_ERROR;

    taskENTER_CRITICAL();
    for (uint32_t i = 0U; i < RESULT_LED_TARGET_COUNT; ++i)
        states[i] = s_states[i];
    taskEXIT_CRITICAL();
    return HAL_OK;
}

void result_led_process(uint32_t now_ms)
{
    uint32_t elapsed;
    uint32_t half_periods;

    if (s_board_map == NULL)
        return;

    taskENTER_CRITICAL();
    elapsed = now_ms - s_last_blink_ms;
    if (elapsed < RESULT_LED_BLINK_MS) {
        taskEXIT_CRITICAL();
        return;
    }

    /*
     * 主循环偶尔延迟超过500ms时按跨过的半周期奇偶数追赶，避免逐次补跳
     * 造成快速闪烁；无符号减法同时覆盖HAL tick约49.7天回绕。
     */
    half_periods = elapsed / RESULT_LED_BLINK_MS;
    s_last_blink_ms += half_periods * RESULT_LED_BLINK_MS;
    if ((half_periods & 1U) != 0U)
        s_blink_on ^= 1U;

    result_led_apply_all();
    taskEXIT_CRITICAL();
}

void result_led_all_off(void)
{
    if (s_board_map == NULL)
        return;

    taskENTER_CRITICAL();
    for (uint32_t i = 0; i < RESULT_LED_TARGET_COUNT; ++i)
        s_states[i] = RESULT_LED_OFF;

    s_blink_on = 0U;
    result_led_apply_all();
    taskEXIT_CRITICAL();
}
