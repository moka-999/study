#include "b2_spi.h"
#include "main.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include <string.h>

/* SPI2/3位于同一DMA请求域。独占Stream3/4，不使用UART已占的Stream0..2。
 * CS边沿才形成事务边界；不使用HAL固定长度完成回调，更不在ISR等待HAL超时。
 */
#if defined(B1_RK_UART)
#define LINK_SPI SPI2
#define LINK_NSS_PORT GPIOB
#define LINK_NSS_PIN GPIO_PIN_12
#define LINK_EXTI_INDEX 12U
#define LINK_EXTI_PORT 1U
#define LINK_RX_REQUEST DMA_REQUEST_SPI2_RX
#define LINK_TX_REQUEST DMA_REQUEST_SPI2_TX
#else
#define LINK_SPI SPI3
#define LINK_NSS_PORT GPIOA
#define LINK_NSS_PIN GPIO_PIN_15
#define LINK_EXTI_INDEX 15U
#define LINK_EXTI_PORT 0U
#define LINK_RX_REQUEST DMA_REQUEST_SPI3_RX
#define LINK_TX_REQUEST DMA_REQUEST_SPI3_TX
#endif
#define DMA_BYTES (B2_MAX_FRAME + 1U) /* 多收一字节，用来明确拒绝超长事务。 */
#define RX_ERRORS (DMA_LISR_TEIF3 | DMA_LISR_DMEIF3)
#define TX_ERRORS (DMA_HISR_TEIF4 | DMA_HISR_DMEIF4)

/* NOLOAD区必须显式初始化；位于AXI SRAM，DMA不可使用DTCM中的普通静态变量。 */
__attribute__((section(".b2_dma"), aligned(32))) static uint8_t s_rx[DMA_BYTES];
__attribute__((section(".b2_dma"), aligned(32))) static uint8_t s_tx[DMA_BYTES];
static DMA_HandleTypeDef s_rx_dma, s_tx_dma;
static SPI_HandleTypeDef s_spi;
static uint32_t s_cfg1, s_cfg2;
static QueueHandle_t s_packets;
static b2_spi_packet_t s_isr_packet;
static volatile uint32_t s_token;
static b2_spi_diagnostics_t s_diagnostics;
static struct {
    uint32_t token;
    size_t length;
    uint8_t bytes[B2_RESPONSE_SIZE];
} s_pending;

static void peripheral_reset(void)
{
#if defined(B1_RK_UART)
    __HAL_RCC_SPI2_FORCE_RESET(); __HAL_RCC_SPI2_RELEASE_RESET();
#else
    __HAL_RCC_SPI3_FORCE_RESET(); __HAL_RCC_SPI3_RELEASE_RESET();
#endif
    LINK_SPI->CFG1 = s_cfg1;
    LINK_SPI->CFG2 = s_cfg2;
    LINK_SPI->CR2 = 0U; /* TSIZE=0：长度由外部NSS界定，不把主机13字节读头当作整帧。 */
    LINK_SPI->UDRDR = 0xFFFFFFFFU;
}

static void arm_transfer(void)
{
    peripheral_reset(); /* 清空上次读剩余的TX FIFO和截断请求的RX FIFO。 */
    DMA1->LIFCR = DMA_LIFCR_CFEIF3 | DMA_LIFCR_CDMEIF3 | DMA_LIFCR_CTEIF3 |
                  DMA_LIFCR_CHTIF3 | DMA_LIFCR_CTCIF3;
    DMA1->HIFCR = DMA_HIFCR_CFEIF4 | DMA_HIFCR_CDMEIF4 | DMA_HIFCR_CTEIF4 |
                  DMA_HIFCR_CHTIF4 | DMA_HIFCR_CTCIF4;
    DMA1_Stream3->PAR = (uint32_t)&LINK_SPI->RXDR;
    DMA1_Stream3->M0AR = (uint32_t)s_rx;
    DMA1_Stream3->NDTR = DMA_BYTES;
    DMA1_Stream4->PAR = (uint32_t)&LINK_SPI->TXDR;
    DMA1_Stream4->M0AR = (uint32_t)s_tx;
    DMA1_Stream4->NDTR = DMA_BYTES;
    __DMB();
    SET_BIT(DMA1_Stream3->CR, DMA_SxCR_EN);
    SET_BIT(DMA1_Stream4->CR, DMA_SxCR_EN);
    SET_BIT(LINK_SPI->CFG1, SPI_CFG1_RXDMAEN | SPI_CFG1_TXDMAEN);
    SET_BIT(LINK_SPI->CR1, SPI_CR1_SPE);
}

/* 仅在NSS高期间停DMA。循环有固定上限，任何硬件异常均丢弃本事务。 */
static int stop_transfer(void)
{
    CLEAR_BIT(LINK_SPI->CFG1, SPI_CFG1_RXDMAEN | SPI_CFG1_TXDMAEN);
    CLEAR_BIT(DMA1_Stream3->CR, DMA_SxCR_EN);
    CLEAR_BIT(DMA1_Stream4->CR, DMA_SxCR_EN);
    for (unsigned int i = 0U; i < 1024U; ++i) {
        if (((DMA1_Stream3->CR | DMA1_Stream4->CR) & DMA_SxCR_EN) == 0U) return 1;
    }
    return 0;
}

/* 输入为同一CS事务的寄存器快照，返回0才允许把请求交给协议层。
 * 直接模式的FEIF不单独证明字节丢失，只作诊断；TE/DME和SPI错误仍拒绝。
 * 若改用FIFO模式，FEIF恢复为致命错误；帧长、HCRC/FCRC由协议层继续检查。
 */
static uint32_t transfer_errors(uint32_t spi_status, uint32_t rx_status,
    uint32_t tx_status, uint32_t rx_fifo_control, uint32_t tx_fifo_control)
{
    uint32_t rx_mask = RX_ERRORS;
    uint32_t tx_mask = TX_ERRORS;
    if ((rx_fifo_control & DMA_SxFCR_DMDIS) != 0U) rx_mask |= DMA_LISR_FEIF3;
    if ((tx_fifo_control & DMA_SxFCR_DMDIS) != 0U) tx_mask |= DMA_HISR_FEIF4;
    return (spi_status & (SPI_SR_OVR | SPI_SR_MODF | SPI_SR_TIFRE)) |
        (rx_status & rx_mask) | (tx_status & tx_mask);
}

void EXTI15_10_IRQHandler(void)
{
    if ((EXTI_D1->PR1 & LINK_NSS_PIN) == 0U) return;
    EXTI_D1->PR1 = LINK_NSS_PIN;
    BaseType_t wake = pdFALSE;
    ++s_diagnostics.transactions;
    int stopped = stop_transfer();
    uint32_t spi_status = LINK_SPI->SR;
    uint32_t rx_status = DMA1->LISR;
    uint32_t tx_status = DMA1->HISR;
    s_diagnostics.last_spi_status = spi_status;
    s_diagnostics.last_rx_dma_status = rx_status;
    s_diagnostics.last_tx_dma_status = tx_status;
    s_diagnostics.last_length = 0U;
    if ((rx_status & DMA_LISR_FEIF3) != 0U || (tx_status & DMA_HISR_FEIF4) != 0U)
        ++s_diagnostics.fifo_events;
    if (!stopped) {
        /* DMA仍运行时绝不能重写地址或复用缓冲；保持SPI关闭并记录，等待人工诊断。 */
        CLEAR_BIT(LINK_SPI->CR1, SPI_CR1_SPE);
        ++s_diagnostics.stop_timeouts;
        ++s_diagnostics.hardware_errors;
        return;
    }
    size_t length = DMA_BYTES - DMA1_Stream3->NDTR;
    uint32_t errors = transfer_errors(spi_status, rx_status, tx_status,
        DMA1_Stream3->FCR, DMA1_Stream4->FCR);
    /* NSS最后一个字节可能尚在RX FIFO；DMA停止后由CPU补收，避免截掉尾标志。 */
    while ((LINK_SPI->SR & SPI_SR_RXP) != 0U && length < DMA_BYTES)
        s_rx[length++] = *((volatile uint8_t *)&LINK_SPI->RXDR);
    s_diagnostics.last_length = (uint32_t)length;
    CLEAR_BIT(LINK_SPI->CR1, SPI_CR1_SPE);
    if (length && s_rx[0] == 0x5AU) {
        ++s_token;
        s_pending.length = 0U;
        memset(s_tx, 0xFF, sizeof(s_tx)); /* 新请求使旧应答失效，不能误领上一事务。 */
        if (!errors && length <= B2_MAX_FRAME) {
            s_isr_packet.token = s_token;
            s_isr_packet.length = (uint16_t)length;
            memcpy(s_isr_packet.data, s_rx, length);
            if (xQueueSendFromISR(s_packets, &s_isr_packet, &wake) != pdPASS)
                ++s_diagnostics.queue_drops;
            else ++s_diagnostics.received_requests;
        } else ++s_diagnostics.hardware_errors;
    } else if (errors || length > B2_MAX_FRAME) {
        ++s_diagnostics.hardware_errors;
    }
    /* 只在一次读结束后切换候选响应。最坏多一次FF轮询，低电平期间帧绝不撕裂。 */
    if (s_pending.length && s_pending.token == s_token) {
        memset(s_tx, 0xFF, sizeof(s_tx));
        memcpy(s_tx, s_pending.bytes, s_pending.length);
        s_pending.length = 0U;
    }
    arm_transfer();
    portYIELD_FROM_ISR(wake);
}

static int configure_dma(DMA_HandleTypeDef *dma, DMA_Stream_TypeDef *stream,
    uint32_t request, uint32_t direction)
{
    dma->Instance = stream;
    dma->Init.Request = request;
    dma->Init.Direction = direction;
    dma->Init.PeriphInc = DMA_PINC_DISABLE;
    dma->Init.MemInc = DMA_MINC_ENABLE;
    dma->Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    dma->Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    dma->Init.Mode = DMA_NORMAL;
    dma->Init.Priority = DMA_PRIORITY_HIGH;
    dma->Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    return HAL_DMA_Init(dma) == HAL_OK;
}

int b2_spi_start(void)
{
    s_packets = xQueueCreate(4U, sizeof(b2_spi_packet_t));
    if (!s_packets) return 0;
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
#if defined(B1_RK_UART)
    __HAL_RCC_SPI2_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    gpio.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &gpio);
#else
    __HAL_RCC_SPI3_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_3 | GPIO_PIN_4;
    gpio.Alternate = GPIO_AF6_SPI3;
    HAL_GPIO_Init(GPIOB, &gpio);
    /* H743的PB5/SPI3_MOSI是AF7，不能照抄PB3/PB4的AF6。 */
    gpio.Pin = GPIO_PIN_5; gpio.Alternate = GPIO_AF7_SPI3;
    HAL_GPIO_Init(GPIOB, &gpio);
    gpio.Pin = GPIO_PIN_15; gpio.Alternate = GPIO_AF6_SPI3;
    HAL_GPIO_Init(GPIOA, &gpio);
#endif
    /* NSS使用弱上拉保证主机未接时不误选；MISO保持硬件NSS控制的三态行为。 */
    gpio.Pin = LINK_NSS_PIN; gpio.Pull = GPIO_PULLUP;
#if defined(B1_RK_UART)
    gpio.Alternate = GPIO_AF5_SPI2;
#endif
    HAL_GPIO_Init(LINK_NSS_PORT, &gpio);
    s_spi.Instance = LINK_SPI;
    s_spi.Init.Mode = SPI_MODE_SLAVE;
    s_spi.Init.Direction = SPI_DIRECTION_2LINES;
    s_spi.Init.DataSize = SPI_DATASIZE_8BIT;
    s_spi.Init.CLKPolarity = SPI_POLARITY_LOW;
    s_spi.Init.CLKPhase = SPI_PHASE_1EDGE;
    s_spi.Init.NSS = SPI_NSS_HARD_INPUT;
    s_spi.Init.FirstBit = SPI_FIRSTBIT_MSB;
    s_spi.Init.FifoThreshold = SPI_FIFO_THRESHOLD_01DATA;
    s_spi.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_DISABLE;
    if (HAL_SPI_Init(&s_spi) != HAL_OK ||
        !configure_dma(&s_rx_dma, DMA1_Stream3, LINK_RX_REQUEST, DMA_PERIPH_TO_MEMORY) ||
        !configure_dma(&s_tx_dma, DMA1_Stream4, LINK_TX_REQUEST, DMA_MEMORY_TO_PERIPH)) return 0;
    s_cfg1 = s_spi.Instance->CFG1 & ~(SPI_CFG1_RXDMAEN | SPI_CFG1_TXDMAEN | SPI_CFG1_UDRCFG);
    s_cfg2 = s_spi.Instance->CFG2;
    memset(s_rx, 0, sizeof(s_rx));
    memset(s_tx, 0xFF, sizeof(s_tx));
    /* AF输入仍可映射EXTI；手动设置仅边沿路由，不将NSS错误改为普通GPIO。 */
    MODIFY_REG(SYSCFG->EXTICR[LINK_EXTI_INDEX / 4U],
        0xFUL << (4U * (LINK_EXTI_INDEX % 4U)),
        LINK_EXTI_PORT << (4U * (LINK_EXTI_INDEX % 4U)));
    CLEAR_BIT(EXTI->FTSR1, LINK_NSS_PIN);
    SET_BIT(EXTI->RTSR1, LINK_NSS_PIN);
    EXTI_D1->PR1 = LINK_NSS_PIN;
    arm_transfer();
    SET_BIT(EXTI_D1->IMR1, LINK_NSS_PIN);
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 6U, 0U);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
    return 1;
}

int b2_spi_receive(b2_spi_packet_t *packet)
{
    return s_packets && xQueueReceive(s_packets, packet, 0U) == pdPASS;
}

int b2_spi_publish(uint32_t token, const uint8_t *frame, size_t length)
{
    if (!frame || !length || length > sizeof(s_pending.bytes)) return 0;
    taskENTER_CRITICAL();
    int current = token == s_token;
    if (current) {
        memcpy(s_pending.bytes, frame, length);
        s_pending.token = token;
        s_pending.length = length;
    }
    taskEXIT_CRITICAL();
    return current;
}

void b2_spi_diagnostics(b2_spi_diagnostics_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL();
    *out = s_diagnostics;
    taskEXIT_CRITICAL();
}
