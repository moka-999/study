#include "board.h"
#include "stm32h7xx_hal.h"
#include <string.h>

static UART_HandleTypeDef s_uart;
static SPI_HandleTypeDef s_spi;
static uint8_t s_byte;
static uint8_t s_rx[256];
static volatile uint16_t s_read, s_write;
static volatile uint8_t s_overflow;

void SysTick_Handler(void) { HAL_IncTick(); }
void USART1_IRQHandler(void) { HAL_UART_IRQHandler(&s_uart); }

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart)
{
    if (uart != &s_uart) return;
    uint16_t next = (uint16_t)((s_write + 1U) % sizeof(s_rx));
    if (next == s_read) s_overflow = 1U;
    else { s_rx[s_write] = s_byte; __DMB(); s_write = next; }
    (void)HAL_UART_Receive_IT(&s_uart, &s_byte, 1U);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
    if (uart != &s_uart) return;
    s_overflow = 1U;
    (void)HAL_UART_Receive_IT(&s_uart, &s_byte, 1U);
}

int master_board_init(void)
{
    HAL_Init();
    if (HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY) != HAL_OK) return 0;
    /* 使用片内HSI 64MHz，避免依赖外接晶振；SPI核时钟同源，/64即1MHz。 */
    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState = RCC_HSI_DIV1;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState = RCC_PLL_OFF;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) return 0;
    RCC_ClkInitTypeDef clock = {0};
    clock.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
        RCC_CLOCKTYPE_D1PCLK1 | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D3PCLK1;
    clock.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    clock.SYSCLKDivider = RCC_SYSCLK_DIV1;
    clock.AHBCLKDivider = RCC_HCLK_DIV1;
    clock.APB3CLKDivider = RCC_APB3_DIV2;
    clock.APB1CLKDivider = RCC_APB1_DIV2;
    clock.APB2CLKDivider = RCC_APB2_DIV2;
    clock.APB4CLKDivider = RCC_APB4_DIV2;
    if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_1) != HAL_OK) return 0;
    RCC_PeriphCLKInitTypeDef peripheral = {0};
    peripheral.PeriphClockSelection = RCC_PERIPHCLK_SPI3 | RCC_PERIPHCLK_USART1 | RCC_PERIPHCLK_CKPER;
    peripheral.CkperClockSelection = RCC_CLKPSOURCE_HSI;
    peripheral.Spi123ClockSelection = RCC_SPI123CLKSOURCE_CLKP;
    peripheral.Usart16ClockSelection = RCC_USART16CLKSOURCE_D2PCLK2;
    if (HAL_RCCEx_PeriphCLKConfig(&peripheral) != HAL_OK) return 0;

    __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE(); __HAL_RCC_SPI3_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_SET);
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_15; gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL; gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Mode = GPIO_MODE_AF_PP; gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Pin = GPIO_PIN_3 | GPIO_PIN_4; gpio.Alternate = GPIO_AF6_SPI3;
    HAL_GPIO_Init(GPIOB, &gpio);
    gpio.Pin = GPIO_PIN_5; gpio.Alternate = GPIO_AF7_SPI3;
    HAL_GPIO_Init(GPIOB, &gpio);
    gpio.Pin = GPIO_PIN_9 | GPIO_PIN_10; gpio.Alternate = GPIO_AF7_USART1;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &gpio);

    s_uart.Instance = USART1;
    s_uart.Init.BaudRate = 115200;
    s_uart.Init.WordLength = UART_WORDLENGTH_8B;
    s_uart.Init.StopBits = UART_STOPBITS_1;
    s_uart.Init.Parity = UART_PARITY_NONE;
    s_uart.Init.Mode = UART_MODE_TX_RX;
    s_uart.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&s_uart) != HAL_OK) return 0;
    HAL_NVIC_SetPriority(USART1_IRQn, 5U, 0U);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
    if (HAL_UART_Receive_IT(&s_uart, &s_byte, 1U) != HAL_OK) return 0;

    s_spi.Instance = SPI3;
    s_spi.Init.Mode = SPI_MODE_MASTER;
    s_spi.Init.Direction = SPI_DIRECTION_2LINES;
    s_spi.Init.DataSize = SPI_DATASIZE_8BIT;
    s_spi.Init.CLKPolarity = SPI_POLARITY_LOW;
    s_spi.Init.CLKPhase = SPI_PHASE_1EDGE;
    s_spi.Init.NSS = SPI_NSS_SOFT;
    s_spi.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_64;
    s_spi.Init.FirstBit = SPI_FIRSTBIT_MSB;
    s_spi.Init.FifoThreshold = SPI_FIFO_THRESHOLD_01DATA;
    s_spi.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;
    return HAL_SPI_Init(&s_spi) == HAL_OK;
}

void console_write(const char *text)
{
    (void)HAL_UART_Transmit(&s_uart, (const uint8_t *)text, (uint16_t)strlen(text), 100U);
}

int console_read(void)
{
    if (s_overflow) {
        __disable_irq(); s_overflow = 0U; s_read = s_write; __enable_irq();
        return -2;
    }
    if (s_read == s_write) return -1;
    uint8_t byte = s_rx[s_read];
    s_read = (uint16_t)((s_read + 1U) % sizeof(s_rx));
    return byte;
}

uint32_t master_millis(void) { return HAL_GetTick(); }
void master_delay(uint32_t ms) { HAL_Delay(ms); }
void master_select(int selected)
{
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, selected ? GPIO_PIN_RESET : GPIO_PIN_SET);
}
int master_transfer(const uint8_t *tx, uint8_t *rx, size_t length)
{
    if (!tx || !rx || !length || length > 1040U) return 0;
    return HAL_SPI_TransmitReceive(&s_spi, tx, rx, (uint16_t)length, 30U) == HAL_OK;
}
