/*
 * STM32 UART适配：任务将载荷复制到TX环，HAL TxISR逐字节发送。
 *
 * UART4专用RK协议，USART2只接收RK日志，USART1输出可选日志并接收本地命令。
 * 三路RX由DMA+IDLE交给路由层；日志不占用协议TX环或协议RX队列。
 * 同一串口的发送任务共用一把互斥锁；排队成功不等于线上已经发完。
 */
#include "uart_hw.h"
#include "board_interfaces.h"
#include "main.h"
#include "FreeRTOS.h"
#include "semphr.h"

#define UART_TX_RING_SIZE       2048U /* 2的幂，实际可用容量为2047字节。 */
#define UART_TX_LOCK_TIMEOUT_MS  100U

typedef struct {
    uint8_t data[UART_TX_RING_SIZE];
    volatile uint16_t head; /* 仅ISR推进：下一字节读取位置。 */
    volatile uint16_t tail; /* 任务持锁推进：下一字节写入位置。 */
    SemaphoreHandle_t mutex;
} uart_tx_ring_t;

static uart_tx_ring_t s_uart4_tx;
static uart_tx_ring_t s_uart1_tx;

void uart_tx_mutex_init(void)
{
    /* 开启接收前调用；重复初始化不泄漏mutex，也不清除已排队数据。 */
    if (s_uart4_tx.mutex == NULL) {
        s_uart4_tx.mutex = xSemaphoreCreateMutex();
    }
    if (s_uart1_tx.mutex == NULL) {
        s_uart1_tx.mutex = xSemaphoreCreateMutex();
    }
}

static uart_tx_ring_t *find_tx_ring(const UART_HandleTypeDef *uart)
{
    if (uart == &huart4) {
        return &s_uart4_tx;
    }
    if (uart == &huart1) {
        return &s_uart1_tx;
    }
    return NULL;
}

/* HAL_UART_IRQHandler在TXE分支调用，不是中断处理完之后额外调用。 */
static void uart_tx_drain(UART_HandleTypeDef *uart)
{
    uart_tx_ring_t *ring = find_tx_ring(uart);
    if (ring == NULL) {
        return;
    }

    USART_TypeDef *instance = uart->Instance;
    while (instance->ISR & USART_ISR_TXE_TXFNF) {
        if (ring->head == ring->tail) {
            instance->CR1 &= ~USART_CR1_TXEIE;
            break;
        }
        instance->TDR = ring->data[ring->head];
        ring->head = (uint16_t)(ring->head + 1U) & (UART_TX_RING_SIZE - 1U);
    }
}

void uart_tx_isr_attach(void)
{
    huart4.TxISR = uart_tx_drain;
    huart1.TxISR = uart_tx_drain;
}

static int uart_queue_send(uart_if_t *port, uint8_t *buffer, uint16_t length)
{
    if (port == NULL || buffer == NULL || length == 0U ||
        length >= UART_TX_RING_SIZE) {
        return IF_ERR;
    }
    UART_HandleTypeDef *uart = port->handle;
    uart_tx_ring_t *ring = find_tx_ring(uart);
    if (ring == NULL || ring->mutex == NULL) {
        return IF_ERR; /* USART2只收无TX环；其余端口锁分配失败也拒绝发送。 */
    }
    /* 控制台在短临界区内串行选择来源，禁止取锁等待；协议口维持原有约束。 */
    TickType_t wait = uart == &huart1 ? 0U : pdMS_TO_TICKS(UART_TX_LOCK_TIMEOUT_MS);
    if (xSemaphoreTake(ring->mutex, wait) != pdTRUE) {
        return IF_ERR;
    }

    /* 整帧空间不足时不复制任何字节，由调用者决定退避、重排或丢弃。 */
    uint16_t available = (uint16_t)(ring->head - ring->tail - 1U) &
                         (UART_TX_RING_SIZE - 1U);
    if (length > available) {
        xSemaphoreGive(ring->mutex);
        return IF_ERR;
    }
    for (uint16_t index = 0U; index < length; ++index) {
        ring->data[ring->tail] = buffer[index];
        ring->tail = (uint16_t)(ring->tail + 1U) & (UART_TX_RING_SIZE - 1U);
    }

    /*
     * 每次发布数据后都唤醒TXE。若只根据复制前的was_empty判断，ISR可能在
     * 两者之间排空旧数据并关闭TXE，使本次新数据滞留。短临界区保护CR1，
     * 并恢复原PRIMASK，不无条件开中断破坏外层状态。
     */
    uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    __HAL_UART_ENABLE_IT(uart, UART_IT_TXE);
    __set_PRIMASK(saved_primask);

    xSemaphoreGive(ring->mutex);
    return IF_OK;
}

/* DMA缓冲的地址/生命周期由路由层保证；这里仅启动一次接收。 */
static int uart_start_receive(uart_if_t *port, uint8_t *buffer, uint16_t size)
{
    if (port == NULL || port->handle == NULL || buffer == NULL || size == 0U) {
        return IF_ERR;
    }
    UART_HandleTypeDef *uart = port->handle;
    /* B1由Core绑定NORMAL RX DMA。缺失DMA时直接拒绝，不能进入HAL解引用。 */
    if (uart->hdmarx == NULL || uart->hdmarx->Init.Mode != DMA_NORMAL) {
        return IF_ERR;
    }
    if (HAL_UARTEx_ReceiveToIdle_DMA(uart, buffer, size) != HAL_OK) {
        return IF_ERR;
    }
    /*
     * 本路由按“本轮接收的完整前缀”交付IDLE/TC数据，不维护循环DMA游标。
     * HT只是半传输通知，DMA仍在收；关闭HT避免同一前缀被重复入队。
     * 每次重启DMA后都要关闭，不能只在首次初始化时设置一次。
     */
    __HAL_DMA_DISABLE_IT(uart->hdmarx, DMA_IT_HT);
    return IF_OK;
}

int uart_rx_abort(uart_if_t *port)
{
    if (port == NULL || port->handle == NULL) return IF_ERR;
    return HAL_UART_AbortReceive(port->handle) == HAL_OK ? IF_OK : IF_ERR;
}

uart_if_t uart4_if = {
    .handle = &huart4,
    .send = uart_queue_send,
    .start_rx = uart_start_receive,
};
uart_if_t uart2_if = {
    .handle = &huart2,
    .send = uart_queue_send,
    .start_rx = uart_start_receive,
};
uart_if_t uart1_if = {
    .handle = &huart1,
    .send = uart_queue_send,
    .start_rx = uart_start_receive,
};
