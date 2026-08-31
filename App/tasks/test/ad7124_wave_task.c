/*
 * 阿波罗 V2 AD7124 SPI2 波形测试任务
 *
 * 本镜像不假定开发板上安装了 AD7124。它只调用正式 AD7124 驱动，循环
 * 输出一组固定、容易解码的事务，用逻辑分析仪确认 SPI Mode 3、CS 时序、
 * MSB 顺序及寄存器命令。MISO 未接从设备时的读回值没有业务意义。
 *
 * 接线：PB12=CS、PB13=SCK、PB15=MOSI、PB14=MISO，并与分析仪共地。
 * 该任务仅在 ENABLE_AD7124_WAVE_TEST 隔离镜像中创建，不进入正式固件。
 */
#include "FreeRTOS.h"
#include "task.h"

#include "ad7124.h"
#include "board_interfaces.h"
#include "main.h"
#include "spi.h"

#if defined(ENABLE_AD7124_WAVE_TEST) && ENABLE_AD7124_WAVE_TEST

#define AD_WAVE_START_DELAY_MS       2000U
#define AD_WAVE_TRANSACTION_GAP_MS    100U
#define AD_WAVE_REPEAT_GAP_MS        2000U

static void ad_wave_wait_ms(uint32_t delay_ms)
{
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static void ad_wave_configure_cs(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_12;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET);
}

static void ad_wave_slow_spi_for_analyzer(void)
{
    /*
     * PLL2给SPI2约129 MHz；硬件最大256分频后约504 kHz。
     * AD7124采样数据量很小，B1正式SPI4也应先采用保守速率；
     * 真板信号质量确认后，再决定是否提高时钟。
     */
    HAL_SPI_DeInit(&hspi2);
    hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256;
    if (HAL_SPI_Init(&hspi2) != HAL_OK)
        Error_Handler();
}

void StartAd7124WaveTask(void *argument)
{
    (void)argument;

    ad_wave_configure_cs();
    ad_wave_slow_spi_for_analyzer();

    ad7124_t adc = {
        .spi = &spi2_if,
        .cs_port = GPIOB,
        .cs_pin = GPIO_PIN_12,
        .wait_ms = ad_wave_wait_ms,
    };

    /* 上电后留2秒静默区，便于逻辑分析仪用CS下降沿触发。 */
    vTaskDelay(pdMS_TO_TICKS(AD_WAVE_START_DELAY_MS));

    for (;;) {
        uint8_t read_data[4];

        /* 64个高电平数据位复位；驱动随后自动读取一次STATUS寄存器。 */
        (void)ad7124_reset(&adc);
        vTaskDelay(pdMS_TO_TICKS(AD_WAVE_TRANSACTION_GAP_MS));

        /* 读ID：MOSI应为45 00；没有AD7124时忽略MISO返回值。 */
        (void)ad7124_read_id(&adc);
        vTaskDelay(pdMS_TO_TICKS(AD_WAVE_TRANSACTION_GAP_MS));

        /* 写ADC_CONTROL：MOSI应为01 05 90（全功耗、内部时钟、空闲模式）。 */
        (void)ad7124_set_mode(&adc, AD7124_MODE_IDLE);
        vTaskDelay(pdMS_TO_TICKS(AD_WAVE_TRANSACTION_GAP_MS));

        /* 配置CH0：MOSI应为09 80 11（AIN0相对AVSS，使用SETUP0）。 */
        (void)ad7124_config_channel(&adc, 0U, 1U, 0U,
                                    AD7124_AIN_AIN0, AD7124_AIN_AVSS);
        vTaskDelay(pdMS_TO_TICKS(AD_WAVE_TRANSACTION_GAP_MS));

        /* 读DATA+STATUS：MOSI应为42 00 00 00 00。 */
        (void)ad7124_read_reg(&adc, AD7124_REG_DATA, read_data, sizeof(read_data));

        vTaskDelay(pdMS_TO_TICKS(AD_WAVE_REPEAT_GAP_MS));
    }
}

#endif
