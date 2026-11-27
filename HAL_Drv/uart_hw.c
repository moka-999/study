/*
 * STM32 UART适配：任务将载荷复制到TX环，HAL TxISR逐字节发送。
 *
 * UART4专用RK协议，USART2双向转发RK调试字符，USART1输出可选日志并接收本地命令。
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

typedef enum {
    UART_TX_UNTRACKED = 0,
    UART_TX_TRACKED_QUEUED,
    UART_TX_TRACKED_DRAINING,
    UART_TX_TRACKED_COMPLETE
} uart_tx_tracking_t;

typedef struct {
    uint8_t data[UART_TX_RING_SIZE];
    volatile uint16_t head; /* 仅ISR推进：下一字节读取位置。 */
    volatile uint16_t tail; /* 任务持锁推进：下一字节写入位置。 */
    SemaphoreHandle_t mutex;
    volatile uint32_t ticket;
    volatile uint32_t completed_ms;
    volatile uint16_t tracked_end;
    volatile uart_tx_tracking_t tracked; /* 排队、等待最后停止位、完成三个跟踪阶段。 */
} uart_tx_ring_t;

static uart_tx_ring_t s_uart4_tx;
static uart_tx_ring_t s_uart1_tx;
static uart_tx_ring_t s_uart2_tx;

void uart_tx_mutex_init(void)
{
    /* 开启接收前调用；重复初始化不泄漏mutex，也不清除已排队数据。 */
    if (s_uart4_tx.mutex == NULL) {
        s_uart4_tx.mutex = xSemaphoreCreateMutex();
    }
    if (s_uart2_tx.mutex == NULL) {
        s_uart2_tx.mutex = xSemaphoreCreateMutex();
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
    if (uart == &huart2) {
        return &s_uart2_tx;
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
        if (ring->tracked == UART_TX_TRACKED_QUEUED && ring->head == ring->tracked_end) {
            /* 不让后续载荷越过本帧TC边界；TC表示移位寄存器也已发完。 */
            instance->CR1 &= ~USART_CR1_TXEIE;
            ring->tracked = UART_TX_TRACKED_DRAINING;
            __HAL_UART_ENABLE_IT(uart, UART_IT_TC);
            break;
        }
    }
}

void uart_tx_isr_attach(void)
{
    huart4.TxISR = uart_tx_drain;
    huart1.TxISR = uart_tx_drain;
    huart2.TxISR = uart_tx_drain;
}

/* HAL结束TC处理时会清TxISR；恢复本驱动钩子后继续发送后续已排队载荷。 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *uart)
{
    uart_tx_ring_t *ring = find_tx_ring(uart);
    if (ring == NULL) return;
    if (ring->tracked == UART_TX_TRACKED_DRAINING) {
        ring->completed_ms = HAL_GetTick();
        ring->tracked = UART_TX_TRACKED_COMPLETE;
    }
    uart->TxISR = uart_tx_drain;
    if (ring->head != ring->tail) __HAL_UART_ENABLE_IT(uart, UART_IT_TXE);
}

int uart_hw_tx_status(uart_if_t *port, uint32_t ticket, uint32_t *completed_ms)
{
    uart_tx_ring_t *ring = port != NULL ? find_tx_ring(port->handle) : NULL;
    if (ring == NULL || ticket == 0U || completed_ms == NULL) return IF_ERR;
    uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    int result = ticket != ring->ticket ? IF_ERR : (ring->tracked == UART_TX_TRACKED_COMPLETE ? 1 : 0);
    if (result == 1) *completed_ms = ring->completed_ms;
    __set_PRIMASK(saved_primask);
    return result;
}

static int queue_send(uart_if_t *port, uint8_t *buffer, uint16_t length,
                       uint32_t *ticket)
{
    if (port == NULL || buffer == NULL || length == 0U ||
        length >= UART_TX_RING_SIZE) {
        return IF_ERR;
    }
    UART_HandleTypeDef *uart = port->handle;
    uart_tx_ring_t *ring = find_tx_ring(uart);
    if (ring == NULL || ring->mutex == NULL) {
        return IF_ERR; /* 端口无发送环或锁分配失败时拒绝发送。 */
    }
    /* 两个调试端口不等待TX锁，避免阻塞默认任务；协议口维持原有约束。 */
    TickType_t wait = uart == &huart4 ? pdMS_TO_TICKS(UART_TX_LOCK_TIMEOUT_MS) : 0U;
    if (xSemaphoreTake(ring->mutex, wait) != pdTRUE) {
        return IF_ERR;
    }

    /* 整帧空间不足时不复制任何字节，由调用者决定退避、重排或丢弃。 */
    uint16_t available = (uint16_t)(ring->head - ring->tail - 1U) &
                         (UART_TX_RING_SIZE - 1U);
    if (length > available || (ticket != NULL && (ring->tracked == UART_TX_TRACKED_QUEUED || ring->tracked == UART_TX_TRACKED_DRAINING))) {
        xSemaphoreGive(ring->mutex);
        return IF_ERR;
    }
    /* 先复制到尚未发布的位置；ISR只能看到旧tail，不能抢先发送本帧。 */
    uint16_t tail = ring->tail;
    for (uint16_t index = 0U; index < length; ++index) {
        ring->data[tail] = buffer[index];
        tail = (uint16_t)(tail + 1U) & (UART_TX_RING_SIZE - 1U);
    }

    /*
     * 每次发布数据后都唤醒TXE。若只根据复制前的was_empty判断，ISR可能在
     * 两者之间排空旧数据并关闭TXE，使本次新数据滞留。短临界区保护CR1，
     * 并恢复原PRIMASK，不无条件开中断破坏外层状态。
     */
    uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    if (ticket != NULL) {
        if (++ring->ticket == 0U) ++ring->ticket;
        *ticket = ring->ticket;
        ring->tracked_end = tail;
        ring->tracked = UART_TX_TRACKED_QUEUED;
    }
    ring->tail = tail;
    if (ring->tracked != UART_TX_TRACKED_DRAINING) __HAL_UART_ENABLE_IT(uart, UART_IT_TXE);
    __set_PRIMASK(saved_primask);

    xSemaphoreGive(ring->mutex);
    return IF_OK;
}

static int uart_queue_send(uart_if_t *port, uint8_t *buffer, uint16_t length)
{
    return queue_send(port, buffer, length, NULL);
}

int uart_hw_send_tracked(uart_if_t *port, uint8_t *buffer, uint16_t length,
                          uint32_t *ticket)
{
    if (ticket == NULL) return IF_ERR;
    return queue_send(port, buffer, length, ticket);
}

/* DMA缓冲的地址/生命周期由路由层保证；这里仅启动一次接收。 */
static int uart_start_receive(uart_if_t *port, uint8_t *buffer, uint16_t size)
{
    if (port == NULL || port->handle == NULL || buffer == NULL || size == 0U) {
        return IF_ERR;
    }
    UART_HandleTypeDef *uart = port->handle;
    /* 两个RK端口连续接收；本地命令口按单次块交付。 */
    uint32_t mode = uart == &huart1 ? DMA_NORMAL : DMA_CIRCULAR;
    if (uart->hdmarx == NULL || uart->hdmarx->Init.Mode != mode) {
        return IF_ERR;
    }
    if (HAL_UARTEx_ReceiveToIdle_DMA(uart, buffer, size) != HAL_OK) {
        return IF_ERR;
    }
    /* 循环DMA保留半满通知，使连续输入在回绕前得到消费。 */
    if (mode == DMA_NORMAL) __HAL_DMA_DISABLE_IT(uart->hdmarx, DMA_IT_HT);
    return IF_OK;
}

uint16_t uart_hw_rx_position(uart_if_t *port, uint16_t size)
{
    UART_HandleTypeDef *uart = port->handle;
    uint32_t left = __HAL_DMA_GET_COUNTER(uart->hdmarx);
    return left && left <= size ? (uint16_t)(size - left) : 0U;
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

/* 交接前证明全部UART发送已完成，不能用最后一帧入队时间代替TC。 */
int uart_hw_tx_idle(uart_if_t *port)
{
    UART_HandleTypeDef *uart = port ? port->handle : NULL;
    uart_tx_ring_t *ring = uart ? find_tx_ring(uart) : NULL;
    if (!ring || !ring->mutex) return 0;
    uint32_t saved = __get_PRIMASK();
    __disable_irq();
    int idle = ring->head == ring->tail && __HAL_UART_GET_FLAG(uart, UART_FLAG_TC) != RESET;
    __set_PRIMASK(saved);
    return idle;
}

/* 不阻塞等TX，也不在临界区调用HAL；保留协议端口的自定义TX中断钩子。 */
uart_baud_result_t uart_hw_set_baud(uart_if_t *port, uint32_t baud)
{
    UART_HandleTypeDef *uart = port ? port->handle : NULL;
    uart_tx_ring_t *ring = uart ? find_tx_ring(uart) : NULL;
    if (!baud || !ring || !ring->mutex || xSemaphoreTake(ring->mutex, 0U) != pdTRUE)
        return UART_BAUD_UNCHANGED;
    uart_baud_result_t result = UART_BAUD_UNCHANGED;
    uint32_t previous = uart->Init.BaudRate;
    if (uart_hw_tx_idle(port)) {
        if (uart_rx_abort(port) != IF_OK) {
            xSemaphoreGive(ring->mutex);
            return UART_BAUD_RX_FAULT;
        }
        uart->Init.BaudRate = baud;
        if (HAL_UART_Init(uart) == HAL_OK) result = UART_BAUD_CHANGED;
        else {
            result = UART_BAUD_RX_STOPPED;
            uart->Init.BaudRate = previous;
            (void)HAL_UART_Init(uart);
        }
        uart->TxISR = uart_tx_drain;
    }
    xSemaphoreGive(ring->mutex);
    return result;
}
