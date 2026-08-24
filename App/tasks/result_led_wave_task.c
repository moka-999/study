/*
 * 阿波罗 V2 结果灯逻辑分析仪测试任务
 *
 * B1 最终使用 PG0-PG5 控制三颗红绿双色灯；Apollo V2 的同名引脚
 * 已被 FMC、触摸和 LCD 等板载网络占用，而且没有完整引出，不能直接复用。
 * 本测试镜像因此使用 P2-23 至 P2-28 六个连续排针作为替代输出：
 *
 *   D0 P2-23 / PD7  = LPDDR4/4X #0 红
 *   D1 P2-24 / PD3  = LPDDR4/4X #0 绿
 *   D2 P2-25 / PD2  = LPDDR4/4X #1 红
 *   D3 P2-26 / PC12 = LPDDR4/4X #1 绿
 *   D4 P2-27 / PC11 = eMCP 红
 *   D5 P2-28 / PC10 = eMCP 绿
 *
 * 六路只输出业务状态转换后的高低电平，不模拟 B1 上 2N7002 的反相电路。
 * 测试时不要插入摄像头、LCD、SD 卡或其他占用这些复用网络的模块。
 */
#include "FreeRTOS.h"
#include "task.h"

#include "led.h"

#if defined(ENABLE_RESULT_LED_WAVE_TEST) && ENABLE_RESULT_LED_WAVE_TEST

#define RESULT_LED_BLINK_MS       500U
#define RESULT_LED_STEADY_MS     2000U
#define RESULT_LED_BLINK_CYCLES     4U

typedef enum {
    RESULT_LED_LPDDR0 = 0,
    RESULT_LED_LPDDR1,
    RESULT_LED_EMCP,
    RESULT_LED_TARGET_COUNT,
} result_led_target_t;

typedef struct {
    GPIO_TypeDef *red_port;
    uint16_t red_pin;
    GPIO_TypeDef *green_port;
    uint16_t green_pin;
} result_led_pin_pair_t;

static const result_led_pin_pair_t s_v2_result_pins[RESULT_LED_TARGET_COUNT] = {
    [RESULT_LED_LPDDR0] = { GPIOD, GPIO_PIN_7, GPIOD, GPIO_PIN_3 },
    [RESULT_LED_LPDDR1] = { GPIOD, GPIO_PIN_2, GPIOC, GPIO_PIN_12 },
    [RESULT_LED_EMCP]   = { GPIOC, GPIO_PIN_11, GPIOC, GPIO_PIN_10 },
};

void result_led_wave_early_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;

    for (uint32_t i = 0; i < RESULT_LED_TARGET_COUNT; ++i) {
        /* 先预置低电平再切换为输出，避免测试开始时产生假亮脉冲。 */
        HAL_GPIO_WritePin(s_v2_result_pins[i].red_port,
                          s_v2_result_pins[i].red_pin, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(s_v2_result_pins[i].green_port,
                          s_v2_result_pins[i].green_pin, GPIO_PIN_RESET);

        gpio.Pin = s_v2_result_pins[i].red_pin;
        HAL_GPIO_Init(s_v2_result_pins[i].red_port, &gpio);
        gpio.Pin = s_v2_result_pins[i].green_pin;
        HAL_GPIO_Init(s_v2_result_pins[i].green_port, &gpio);
    }
}

static void result_led_wave_write(result_led_target_t target,
                                  led_result_state_t state,
                                  uint8_t blink_on)
{
    led_result_levels_t levels;

    if (target >= RESULT_LED_TARGET_COUNT)
        return;

    levels = led_result_render(state, blink_on);
    HAL_GPIO_WritePin(s_v2_result_pins[target].red_port,
                      s_v2_result_pins[target].red_pin,
                      levels.red_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(s_v2_result_pins[target].green_port,
                      s_v2_result_pins[target].green_pin,
                      levels.green_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void result_led_wave_all_off(void)
{
    for (uint32_t i = 0; i < RESULT_LED_TARGET_COUNT; ++i)
        result_led_wave_write((result_led_target_t)i, LED_RESULT_OFF, 0U);
}

static void result_led_wave_blink(result_led_target_t first,
                                  result_led_target_t last,
                                  led_result_state_t state)
{
    for (uint32_t cycle = 0; cycle < RESULT_LED_BLINK_CYCLES; ++cycle) {
        for (uint32_t i = first; i <= last; ++i)
            result_led_wave_write((result_led_target_t)i, state, 1U);
        vTaskDelay(pdMS_TO_TICKS(RESULT_LED_BLINK_MS));

        for (uint32_t i = first; i <= last; ++i)
            result_led_wave_write((result_led_target_t)i, state, 0U);
        vTaskDelay(pdMS_TO_TICKS(RESULT_LED_BLINK_MS));
    }
}

void StartResultLedWaveTask(void *argument)
{
    (void)argument;
    /*
     * GPIO阶段已经做过一次上电安全初始化；这里再次执行，避免测试任务被
     * 单独复用或后续初始化代码改写引脚模式时留下不可控状态。
     */
    result_led_wave_early_init();

    /* 预留两秒全低窗口，便于断电重上电采集并检查启动毛刺。 */
    vTaskDelay(pdMS_TO_TICKS(2000U));

    for (;;) {
        result_led_wave_all_off();
        vTaskDelay(pdMS_TO_TICKS(1000U));

        /* 依次验证三组物理通道和四种已冻结的业务显示语义。 */
        result_led_wave_write(RESULT_LED_LPDDR0, LED_RESULT_PASS, 1U);
        vTaskDelay(pdMS_TO_TICKS(RESULT_LED_STEADY_MS));
        result_led_wave_all_off();

        result_led_wave_write(RESULT_LED_LPDDR1, LED_RESULT_FAIL, 1U);
        vTaskDelay(pdMS_TO_TICKS(RESULT_LED_STEADY_MS));
        result_led_wave_all_off();

        result_led_wave_blink(RESULT_LED_LPDDR0, RESULT_LED_EMCP,
                              LED_RESULT_TEST_NORMAL);
        result_led_wave_blink(RESULT_LED_LPDDR0, RESULT_LED_EMCP,
                              LED_RESULT_TEST_HOT);
    }
}

#endif
