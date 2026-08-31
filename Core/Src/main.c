#include "main.h"
#include "board_identity.h"
#include "flash_bsp.h"
#include "app_freertos.h"
#include "result_led.h"
#include "pwr_ctrl.h"
#include "cmsis_os2.h"
#include <string.h>

I2C_HandleTypeDef hi2c1;
I2C_HandleTypeDef hi2c2;
SPI_HandleTypeDef hspi2;
SPI_HandleTypeDef hspi4;
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart4;
DMA_HandleTypeDef hdma_uart4_rx;
DMA_HandleTypeDef hdma_usart2_rx;
QSPI_HandleTypeDef hqspi;

uint8_t g_b1_mac[6];

static void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C_Init(void);
static void MX_SPI_Init(void);
static void MX_UART_Init(void);
static void MX_QSPI_Init(void);

/*
 * 原理图只冻结了LED1/2、LED3/4、LED5/6为三组双色灯，尚未冻结每组
 * 对应的测试对象和红绿顺序。当前验收镜像暂按奇数红、偶数绿以及
 * LPDDR0、LPDDR1、eMCP的顺序映射；B1真板目视确认后只修改本表。
 */
static const result_led_board_map_t s_result_led_map = {
    .pairs = {
        [RESULT_LED_LPDDR0] = { GPIOG, B1_LED1_PIN, GPIOG, B1_LED2_PIN },
        [RESULT_LED_LPDDR1] = { GPIOG, B1_LED3_PIN, GPIOG, B1_LED4_PIN },
        [RESULT_LED_EMCP]   = { GPIOG, B1_LED5_PIN, GPIOG, B1_LED6_PIN },
    },
};

static void require_ok(HAL_StatusTypeDef status)
{
    if (status != HAL_OK) Error_Handler();
}

int main(void)
{
    static const uint8_t banner[] = "B1 gateway software ready; power remains OFF\r\n";

    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    /* 只建立安全OFF基线；时序未冻结前不允许上电。 */
    pwr_ctrl_init(NULL);
    MX_I2C_Init();
    MX_SPI_Init();
    MX_UART_Init();
    MX_QSPI_Init();
    /* 只初始化并确认QSPI通信，不在启动时自动擦写版本或FRU生产区域。 */
    if (flash_if.init(&flash_if) != IF_OK) Error_Handler();
    require_ok(result_led_init(&s_result_led_map));

    memcpy(g_b1_mac, board_mac_address(), sizeof(g_b1_mac));
    HAL_GPIO_WritePin(B1_ETH_NRST_PORT, B1_ETH_NRST_PIN, GPIO_PIN_RESET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(B1_ETH_NRST_PORT, B1_ETH_NRST_PIN, GPIO_PIN_SET);
    HAL_Delay(100);
    require_ok(HAL_UART_Transmit(&huart1, (uint8_t *)banner, sizeof(banner) - 1, 100));

    /* 网络/RK/监测任务在RTOS中运行；这里不配置上电时序、不写Boot电压。 */
    osKernelInitialize();
    MX_FREERTOS_Init();
    osKernelStart();

    /* 调度器正常启动后不会返回。 */
    Error_Handler();
    return 0;
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
    HAL_GPIO_WritePin(B1_SPI4_NCS_PORT, B1_SPI4_NCS_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOH, B1_I2C4_SCL_PIN | B1_I2C4_SDA_PIN, GPIO_PIN_SET);

    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Pin = B1_PWR_4V0_EN_PIN | B1_PWR_3V3_EN_PIN;
    HAL_GPIO_Init(GPIOC, &gpio);
    gpio.Pin = B1_PWR_DDR_EN_PIN | B1_ETH_NRST_PIN |
               B1_LED1_PIN | B1_LED2_PIN | B1_LED3_PIN | B1_LED4_PIN |
               B1_LED5_PIN | B1_LED6_PIN | B1_LED7_PIN;
    HAL_GPIO_Init(GPIOG, &gpio);
    gpio.Pin = B1_SPI4_NCS_PIN;
    HAL_GPIO_Init(B1_SPI4_NCS_PORT, &gpio);

    /* PH4/PH5 are the schematic's logical I2C4 bus, but not an I2C4 AF pair. */
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Pin = B1_I2C4_SCL_PIN | B1_I2C4_SDA_PIN;
    HAL_GPIO_Init(GPIOH, &gpio);

    gpio_af(GPIOA, B1_RMII_REF_CLK_PIN | B1_ETH_MDIO_PIN | B1_RMII_CRS_DV_PIN,
            GPIO_AF11_ETH, GPIO_MODE_AF_PP);
    gpio_af(GPIOB, B1_RMII_TX_EN_PIN, GPIO_AF11_ETH, GPIO_MODE_AF_PP);
    gpio_af(GPIOC, B1_ETH_MDC_PIN | B1_RMII_RXD0_PIN | B1_RMII_RXD1_PIN,
            GPIO_AF11_ETH, GPIO_MODE_AF_PP);
    gpio_af(GPIOG, B1_RMII_TXD0_PIN | B1_RMII_TXD1_PIN, GPIO_AF11_ETH, GPIO_MODE_AF_PP);

    gpio_af(GPIOB, B1_I2C1_SDA_PIN | B1_I2C1_SCL_PIN, GPIO_AF4_I2C1, GPIO_MODE_AF_OD);
    gpio_af(GPIOF, B1_I2C2_SDA_PIN | B1_I2C2_SCL_PIN, GPIO_AF4_I2C2, GPIO_MODE_AF_OD);
    gpio_af(GPIOB, B1_SPI2_NCS_PIN | B1_SPI2_SCK_PIN | B1_SPI2_MISO_PIN | B1_SPI2_MOSI_PIN,
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
    i2c_init(&hi2c1, I2C1);
    i2c_init(&hi2c2, I2C2);
}

static void spi_init(SPI_HandleTypeDef *spi, SPI_TypeDef *instance, uint32_t mode,
                     uint32_t nss, uint32_t prescaler, uint32_t polarity, uint32_t phase)
{
    spi->Instance = instance;
    spi->Init.Mode = mode;
    spi->Init.Direction = SPI_DIRECTION_2LINES;
    spi->Init.DataSize = SPI_DATASIZE_8BIT;
    spi->Init.CLKPolarity = polarity;
    spi->Init.CLKPhase = phase;
    spi->Init.NSS = nss;
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
    spi->Init.MasterKeepIOState = mode == SPI_MODE_MASTER ?
                                  SPI_MASTER_KEEP_IO_STATE_ENABLE :
                                  SPI_MASTER_KEEP_IO_STATE_DISABLE;
    spi->Init.IOSwap = SPI_IO_SWAP_DISABLE;
    require_ok(HAL_SPI_Init(spi));
}

static void MX_SPI_Init(void)
{
    __HAL_RCC_SPI2_CLK_ENABLE();
    __HAL_RCC_SPI4_CLK_ENABLE();
    /* B2 drives the shared bus; 1 MHz is the B2 bring-up clock, not a B1 divider. */
    spi_init(&hspi2, SPI2, SPI_MODE_SLAVE, SPI_NSS_HARD_INPUT,
             SPI_BAUDRATEPRESCALER_2,
             SPI_POLARITY_LOW, SPI_PHASE_1EDGE);
    /*
     * AD7124使用Mode 3。SPI4内核为100MHz，256分频约390.6kHz；
     * 首版优先保证B1布线和器件时序裕量，真板确认后再评估是否提速。
     */
    spi_init(&hspi4, SPI4, SPI_MODE_MASTER, SPI_NSS_SOFT,
             SPI_BAUDRATEPRESCALER_256,
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

/*
 * 两路候选RK串口各配NORMAL RX DMA；运行时只注册选定的一路。
 * RX DMA在调度器启动后的默认任务中才开启，避免FromISR早于内核运行。
 * TX复用软件环+TXE中断，不占用DMA Stream；USART1仍仅作本地控制台。
 */
static void uart_rx_dma_init(UART_HandleTypeDef *uart, DMA_HandleTypeDef *dma,
                              DMA_Stream_TypeDef *stream, uint32_t request)
{
    dma->Instance = stream;
    dma->Init.Request = request;
    dma->Init.Direction = DMA_PERIPH_TO_MEMORY;
    dma->Init.PeriphInc = DMA_PINC_DISABLE;
    dma->Init.MemInc = DMA_MINC_ENABLE;
    dma->Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    dma->Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    dma->Init.Mode = DMA_NORMAL;
    dma->Init.Priority = DMA_PRIORITY_HIGH;
    dma->Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    require_ok(HAL_DMA_Init(dma));
    __HAL_LINKDMA(uart, hdmarx, *dma);
}

static void MX_UART_Init(void)
{
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_UART4_CLK_ENABLE();
    uart_init(&huart1, USART1);
    uart_init(&huart2, USART2);
    uart_init(&huart4, UART4);
    __HAL_RCC_DMA1_CLK_ENABLE();
    uart_rx_dma_init(&huart4, &hdma_uart4_rx, DMA1_Stream0, DMA_REQUEST_UART4_RX);
    uart_rx_dma_init(&huart2, &hdma_usart2_rx, DMA1_Stream1, DMA_REQUEST_USART2_RX);

    /* 回调使用FreeRTOS FromISR接口，优先级不得高于可调用内核的5级边界。 */
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 5, 0);
    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 5, 0);
    HAL_NVIC_SetPriority(UART4_IRQn, 5, 0);
    HAL_NVIC_SetPriority(USART2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
    HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);
    HAL_NVIC_EnableIRQ(UART4_IRQn);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
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
