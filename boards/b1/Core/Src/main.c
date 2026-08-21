#include "main.h"
#include "board_identity.h"
#include <string.h>

I2C_HandleTypeDef hi2c1;
I2C_HandleTypeDef hi2c2;
I2C_HandleTypeDef hi2c4;
SPI_HandleTypeDef hspi2;
SPI_HandleTypeDef hspi4;
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart4;
QSPI_HandleTypeDef hqspi;

uint8_t g_b1_mac[6];

static void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C_Init(void);
static void MX_SPI_Init(void);
static void MX_UART_Init(void);
static void MX_QSPI_Init(void);

static void require_ok(HAL_StatusTypeDef status)
{
    if (status != HAL_OK) Error_Handler();
}

int main(void)
{
    static const uint16_t led_pins[] = {
        B1_LED1_PIN, B1_LED2_PIN, B1_LED3_PIN, B1_LED4_PIN,
        B1_LED5_PIN, B1_LED6_PIN, B1_LED7_PIN
    };
    static const uint8_t banner[] = "B1 final-pin check ready\r\n";

    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_I2C_Init();
    MX_SPI_Init();
    MX_UART_Init();
    MX_QSPI_Init();

    memcpy(g_b1_mac, board_mac_address(), sizeof(g_b1_mac));
    HAL_GPIO_WritePin(B1_ETH_NRST_PORT, B1_ETH_NRST_PIN, GPIO_PIN_RESET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(B1_ETH_NRST_PORT, B1_ETH_NRST_PIN, GPIO_PIN_SET);
    HAL_Delay(100);
    require_ok(HAL_UART_Transmit(&huart1, (uint8_t *)banner, sizeof(banner) - 1, 100));

    for (;;) {
        for (uint32_t i = 0; i < sizeof(led_pins) / sizeof(led_pins[0]); i++) {
            HAL_GPIO_WritePin(GPIOG, led_pins[i], GPIO_PIN_SET);
            HAL_Delay(500);
            HAL_GPIO_WritePin(GPIOG, led_pins[i], GPIO_PIN_RESET);
        }
    }
}

void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
}

static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM = 5;
    osc.PLL.PLLN = 160;
    osc.PLL.PLLP = 2;
    osc.PLL.PLLQ = 4;
    osc.PLL.PLLR = 2;
    osc.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
    osc.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
    require_ok(HAL_RCC_OscConfig(&osc));

    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
                    RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.SYSCLKDivider = RCC_SYSCLK_DIV1;
    clk.AHBCLKDivider = RCC_HCLK_DIV2;
    clk.APB3CLKDivider = RCC_APB3_DIV2;
    clk.APB1CLKDivider = RCC_APB1_DIV2;
    clk.APB2CLKDivider = RCC_APB2_DIV2;
    clk.APB4CLKDivider = RCC_APB4_DIV2;
    require_ok(HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2));
}

static void gpio_af(GPIO_TypeDef *port, uint32_t pins, uint32_t af, uint32_t mode)
{
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = pins;
    gpio.Mode = mode;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = af;
    HAL_GPIO_Init(port, &gpio);
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();

    HAL_GPIO_WritePin(GPIOC, B1_PWR_4V0_EN_PIN | B1_PWR_3V3_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOG, B1_PWR_DDR_EN_PIN | B1_ETH_NRST_PIN |
                            B1_LED1_PIN | B1_LED2_PIN | B1_LED3_PIN | B1_LED4_PIN |
                            B1_LED5_PIN | B1_LED6_PIN | B1_LED7_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(B1_SPI2_NCS_PORT, B1_SPI2_NCS_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(B1_SPI4_NCS_PORT, B1_SPI4_NCS_PIN, GPIO_PIN_SET);

    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Pin = B1_PWR_4V0_EN_PIN | B1_PWR_3V3_EN_PIN;
    HAL_GPIO_Init(GPIOC, &gpio);
    gpio.Pin = B1_PWR_DDR_EN_PIN | B1_ETH_NRST_PIN |
               B1_LED1_PIN | B1_LED2_PIN | B1_LED3_PIN | B1_LED4_PIN |
               B1_LED5_PIN | B1_LED6_PIN | B1_LED7_PIN;
    HAL_GPIO_Init(GPIOG, &gpio);
    gpio.Pin = B1_SPI2_NCS_PIN;
    HAL_GPIO_Init(B1_SPI2_NCS_PORT, &gpio);
    gpio.Pin = B1_SPI4_NCS_PIN;
    HAL_GPIO_Init(B1_SPI4_NCS_PORT, &gpio);

    gpio_af(GPIOA, B1_RMII_REF_CLK_PIN | B1_ETH_MDIO_PIN | B1_RMII_CRS_DV_PIN,
            GPIO_AF11_ETH, GPIO_MODE_AF_PP);
    gpio_af(GPIOB, B1_RMII_TX_EN_PIN, GPIO_AF11_ETH, GPIO_MODE_AF_PP);
    gpio_af(GPIOC, B1_ETH_MDC_PIN | B1_RMII_RXD0_PIN | B1_RMII_RXD1_PIN,
            GPIO_AF11_ETH, GPIO_MODE_AF_PP);
    gpio_af(GPIOG, B1_RMII_TXD0_PIN | B1_RMII_TXD1_PIN, GPIO_AF11_ETH, GPIO_MODE_AF_PP);

    gpio_af(GPIOB, B1_I2C1_SDA_PIN | B1_I2C1_SCL_PIN, GPIO_AF4_I2C1, GPIO_MODE_AF_OD);
    gpio_af(GPIOF, B1_I2C2_SDA_PIN | B1_I2C2_SCL_PIN, GPIO_AF4_I2C2, GPIO_MODE_AF_OD);
    gpio_af(GPIOH, B1_I2C4_SCL_PIN | B1_I2C4_SDA_PIN, GPIO_AF4_I2C4, GPIO_MODE_AF_OD);

    gpio_af(GPIOB, B1_SPI2_SCK_PIN | B1_SPI2_MISO_PIN | B1_SPI2_MOSI_PIN,
            GPIO_AF5_SPI2, GPIO_MODE_AF_PP);
    gpio_af(GPIOE, B1_SPI4_SCK_PIN | B1_SPI4_MISO_PIN | B1_SPI4_MOSI_PIN,
            GPIO_AF5_SPI4, GPIO_MODE_AF_PP);

    gpio_af(GPIOA, B1_USART1_TX_PIN | B1_USART1_RX_PIN, GPIO_AF7_USART1, GPIO_MODE_AF_PP);
    gpio_af(GPIOA, B1_UART4_RX_PIN | B1_UART4_TX_PIN, GPIO_AF6_UART4, GPIO_MODE_AF_PP);
    gpio_af(GPIOD, B1_USART2_TX_PIN | B1_USART2_RX_PIN, GPIO_AF7_USART2, GPIO_MODE_AF_PP);

    gpio_af(GPIOB, B1_QSPI_CLK_PIN, GPIO_AF9_QUADSPI, GPIO_MODE_AF_PP);
    gpio_af(GPIOB, B1_QSPI_NCS_PIN, GPIO_AF10_QUADSPI, GPIO_MODE_AF_PP);
    gpio_af(GPIOF, B1_QSPI_IO3_PIN | B1_QSPI_IO2_PIN, GPIO_AF9_QUADSPI, GPIO_MODE_AF_PP);
    gpio_af(GPIOF, B1_QSPI_IO0_PIN | B1_QSPI_IO1_PIN, GPIO_AF10_QUADSPI, GPIO_MODE_AF_PP);
}

static void i2c_init(I2C_HandleTypeDef *i2c, I2C_TypeDef *instance)
{
    i2c->Instance = instance;
    i2c->Init.Timing = 0x10C0ECFF;
    i2c->Init.OwnAddress1 = 0;
    i2c->Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    i2c->Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    i2c->Init.OwnAddress2 = 0;
    i2c->Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    i2c->Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    i2c->Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    require_ok(HAL_I2C_Init(i2c));
    require_ok(HAL_I2CEx_ConfigAnalogFilter(i2c, I2C_ANALOGFILTER_ENABLE));
}

static void MX_I2C_Init(void)
{
    __HAL_RCC_I2C1_CLK_ENABLE();
    __HAL_RCC_I2C2_CLK_ENABLE();
    __HAL_RCC_I2C4_CLK_ENABLE();
    i2c_init(&hi2c1, I2C1);
    i2c_init(&hi2c2, I2C2);
    i2c_init(&hi2c4, I2C4);
}

static void spi_init(SPI_HandleTypeDef *spi, SPI_TypeDef *instance, uint32_t prescaler,
                     uint32_t polarity, uint32_t phase)
{
    spi->Instance = instance;
    spi->Init.Mode = SPI_MODE_MASTER;
    spi->Init.Direction = SPI_DIRECTION_2LINES;
    spi->Init.DataSize = SPI_DATASIZE_8BIT;
    spi->Init.CLKPolarity = polarity;
    spi->Init.CLKPhase = phase;
    spi->Init.NSS = SPI_NSS_SOFT;
    spi->Init.BaudRatePrescaler = prescaler;
    spi->Init.FirstBit = SPI_FIRSTBIT_MSB;
    spi->Init.TIMode = SPI_TIMODE_DISABLE;
    spi->Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    spi->Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    spi->Init.NSSPolarity = SPI_NSS_POLARITY_LOW;
    spi->Init.FifoThreshold = SPI_FIFO_THRESHOLD_01DATA;
    spi->Init.TxCRCInitializationPattern = SPI_CRC_INITIALIZATION_ALL_ZERO_PATTERN;
    spi->Init.RxCRCInitializationPattern = SPI_CRC_INITIALIZATION_ALL_ZERO_PATTERN;
    spi->Init.MasterSSIdleness = SPI_MASTER_SS_IDLENESS_00CYCLE;
    spi->Init.MasterInterDataIdleness = SPI_MASTER_INTERDATA_IDLENESS_00CYCLE;
    spi->Init.MasterReceiverAutoSusp = SPI_MASTER_RX_AUTOSUSP_DISABLE;
    spi->Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;
    spi->Init.IOSwap = SPI_IO_SWAP_DISABLE;
    require_ok(HAL_SPI_Init(spi));
}

static void MX_SPI_Init(void)
{
    __HAL_RCC_SPI2_CLK_ENABLE();
    __HAL_RCC_SPI4_CLK_ENABLE();
    /* B1-B2 mode/rate are bring-up defaults until the B2 protocol is frozen. */
    spi_init(&hspi2, SPI2, SPI_BAUDRATEPRESCALER_64,
             SPI_POLARITY_LOW, SPI_PHASE_1EDGE);
    /* AD7124 requires SPI mode 3. */
    spi_init(&hspi4, SPI4, SPI_BAUDRATEPRESCALER_16,
             SPI_POLARITY_HIGH, SPI_PHASE_2EDGE);
}

static void uart_init(UART_HandleTypeDef *uart, USART_TypeDef *instance)
{
    uart->Instance = instance;
    uart->Init.BaudRate = 115200;
    uart->Init.WordLength = UART_WORDLENGTH_8B;
    uart->Init.StopBits = UART_STOPBITS_1;
    uart->Init.Parity = UART_PARITY_NONE;
    uart->Init.Mode = UART_MODE_TX_RX;
    uart->Init.HwFlowCtl = UART_HWCONTROL_NONE;
    uart->Init.OverSampling = UART_OVERSAMPLING_16;
    uart->Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    uart->Init.ClockPrescaler = UART_PRESCALER_DIV1;
    require_ok(HAL_UART_Init(uart));
}

static void MX_UART_Init(void)
{
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_UART4_CLK_ENABLE();
    uart_init(&huart1, USART1);
    uart_init(&huart2, USART2);
    uart_init(&huart4, UART4);
}

static void MX_QSPI_Init(void)
{
    __HAL_RCC_QSPI_CLK_ENABLE();
    hqspi.Instance = QUADSPI;
    hqspi.Init.ClockPrescaler = 4;
    hqspi.Init.FifoThreshold = 4;
    hqspi.Init.SampleShifting = QSPI_SAMPLE_SHIFTING_HALFCYCLE;
    hqspi.Init.FlashSize = 24;
    hqspi.Init.ChipSelectHighTime = QSPI_CS_HIGH_TIME_3_CYCLE;
    hqspi.Init.ClockMode = QSPI_CLOCK_MODE_3;
    hqspi.Init.FlashID = QSPI_FLASH_ID_1;
    hqspi.Init.DualFlash = QSPI_DUALFLASH_DISABLE;
    require_ok(HAL_QSPI_Init(&hqspi));
}

void Error_Handler(void)
{
    __disable_irq();
    for (;;) {}
}
