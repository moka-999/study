/**
 * @file result_led.c
 * @brief B1 三颗红绿双色结果灯驱动
 *
 * 原理图中PG0-PG5经1k电阻驱动2N7002栅极，MOS管导通后把LED阴极拉低，
 * 因此GPIO高电平表示点亮。闪烁采用500ms全局相位，三颗灯同时进入测试
 * 状态时会保持同步；本文件不使用HAL_Delay，不阻塞后续网关任务。
 */

#include "result_led.h"

typedef struct {
    uint8_t red_on;
    uint8_t green_on;
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
        levels.green_on = 1U;
        break;

    case RESULT_LED_FAIL:
        levels.red_on = 1U;
        break;

    case RESULT_LED_TEST_NORMAL:
        /* 红绿同时点亮形成橙色，灭相位也必须同时关闭。 */
        levels.red_on = blink_on ? 1U : 0U;
        levels.green_on = levels.red_on;
        break;

    case RESULT_LED_TEST_HOT:
        levels.red_on = blink_on ? 1U : 0U;
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

    HAL_GPIO_WritePin(pair->red_port, pair->red_pin,
                      levels.red_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(pair->green_port, pair->green_pin,
                      levels.green_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
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

    s_states[target] = state;

    /* 新进入闪烁状态时立即点亮，避免最多500ms的首次显示延迟。 */
    if (result_led_is_blinking(state)) {
        s_blink_on = 1U;
        s_last_blink_ms = HAL_GetTick();
        result_led_apply_all();
    } else {
        result_led_apply(target);
    }

    return HAL_OK;
}

void result_led_process(uint32_t now_ms)
{
    uint32_t elapsed;
    uint32_t half_periods;

    if (s_board_map == NULL)
        return;

    elapsed = now_ms - s_last_blink_ms;
    if (elapsed < RESULT_LED_BLINK_MS)
        return;

    /*
     * 主循环偶尔延迟超过500ms时按跨过的半周期奇偶数追赶，避免逐次补跳
     * 造成快速闪烁；无符号减法同时覆盖HAL tick约49.7天回绕。
     */
    half_periods = elapsed / RESULT_LED_BLINK_MS;
    s_last_blink_ms += half_periods * RESULT_LED_BLINK_MS;
    if ((half_periods & 1U) != 0U)
        s_blink_on ^= 1U;

    result_led_apply_all();
}

void result_led_all_off(void)
{
    if (s_board_map == NULL)
        return;

    for (uint32_t i = 0; i < RESULT_LED_TARGET_COUNT; ++i)
        s_states[i] = RESULT_LED_OFF;

    s_blink_on = 0U;
    result_led_apply_all();
}
