/*
 * UART字节路由：ISR只复制接收块/登记异常，任务取块并恢复RX。
 * 不解释PC/RK协议；断流以UART_RX_GAP通知唯一消费者重置组帧状态。
 */
#include "uart_router.h"
#include "uart_hw.h"
#include "main.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include <string.h>

#define UART_SLOT_COUNT       2
#define UART_RX_QUEUE_LENGTH  8U
#define UART_RX_RETRY_MS       100U

typedef struct {
    uint16_t length;
    uint8_t data[UART_RX_BUF_SIZE];
} uart_rx_message_t;

typedef struct {
    uart_if_t *interface;
    QueueHandle_t rx_queue;
    uart_rx_diagnostics_t diagnostics;
    volatile uint8_t gap_pending;
    volatile uint8_t recovery_pending;
    TickType_t last_retry;
} uart_slot_t;

/* 管理状态必须由.bss启动清零；只有DMA载荷放入NOLOAD的.uart_dma。 */
static uart_slot_t s_slots[UART_SLOT_COUNT];
static uint8_t s_rx_buffers[UART_SLOT_COUNT][UART_RX_BUF_SIZE]
    __attribute__((section(".uart_dma")));

static int slot_is_registered(int id)
{
    return id >= 0 && id < UART_SLOT_COUNT &&
           s_slots[id].interface != NULL && s_slots[id].rx_queue != NULL;
}

/* 诊断累计饱和，不回绕成0；任务侧调用时必须屏蔽本路UART/DMA中断。 */
static void count_add(uint32_t *counter, uint32_t amount)
{
    *counter = amount > UINT32_MAX - *counter ? UINT32_MAX : *counter + amount;
}

static void request_recovery(uart_slot_t *slot)
{
    slot->gap_pending = 1U;
    slot->recovery_pending = 1U;
}

/* 仅ISR、调度前或任务临界区调用；失败不能让接收永久停在无人处理的状态。 */
static void start_receive(int id)
{
    uart_slot_t *slot = &s_slots[id];
    if (slot->interface->start_rx(slot->interface, s_rx_buffers[id],
                                  UART_RX_BUF_SIZE) != IF_OK) {
        count_add(&slot->diagnostics.restart_failures, 1U);
        request_recovery(slot);
    }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *uart, uint16_t size)
{
    if (uart == NULL || HAL_UARTEx_GetRxEventType(uart) == HAL_UART_RXEVENT_HT)
        return; /* HT只是接收前缀，不能交付或重启；IDLE/TC才代表本轮结束。 */

    for (int id = 0; id < UART_SLOT_COUNT; ++id) {
        if (!slot_is_registered(id) || s_slots[id].interface->handle != uart)
            continue;
        uart_slot_t *slot = &s_slots[id];
        if (size == 0U || size > UART_RX_BUF_SIZE) {
            count_add(&slot->diagnostics.invalid_events, 1U);
            request_recovery(slot);
            return;
        }
        if (slot->recovery_pending) {
            count_add(&slot->diagnostics.dropped_blocks, 1U);
            slot->gap_pending = 1U;
            return; /* 不把故障期间收到的块交给组帧，也不在ISR执行阻塞Abort。 */
        }

        uart_rx_message_t message;
        message.length = size;
        memcpy(message.data, s_rx_buffers[id], size);
        BaseType_t higher_priority_task_woken = pdFALSE;
        if (xQueueSendFromISR(slot->rx_queue, &message,
                              &higher_priority_task_woken) != pdTRUE) {
            count_add(&slot->diagnostics.queue_overflows, 1U);
            count_add(&slot->diagnostics.dropped_blocks, 1U);
            slot->gap_pending = 1U;
        }
        start_receive(id);
        portYIELD_FROM_ISR(higher_priority_task_woken);
        return;
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
    if (uart == NULL) return;
    for (int id = 0; id < UART_SLOT_COUNT; ++id) {
        if (!slot_is_registered(id) || s_slots[id].interface->handle != uart)
            continue;
        uart_slot_t *slot = &s_slots[id];
        count_add(&slot->diagnostics.errors, 1U);
        slot->diagnostics.last_hal_error = uart->ErrorCode;
        request_recovery(slot); /* 保留HAL错误码；RX停止/清错/重启由任务处理。 */
        return;
    }
}

void uart_register(int id, uart_if_t *interface)
{
    if (id < 0 || id >= UART_SLOT_COUNT || interface == NULL ||
        interface->handle == NULL || interface->start_rx == NULL ||
        interface->send == NULL || s_slots[id].rx_queue != NULL)
        return;

    QueueHandle_t queue = xQueueCreate(UART_RX_QUEUE_LENGTH, sizeof(uart_rx_message_t));
    if (queue == NULL) return; /* 分配失败保持未注册；无空队列解引用。 */
    s_slots[id].interface = interface;
    s_slots[id].rx_queue = queue;
}

void uart_start_all(void)
{
    uart_tx_mutex_init();
    uart_tx_isr_attach();
    for (int id = 0; id < UART_SLOT_COUNT; ++id) {
        if (slot_is_registered(id)) start_receive(id);
    }
}

/* 唯一接收任务驱动恢复；每槽最多100ms尝试一次，避免硬故障时忙重试。
 * Abort只停止RX，可能使用HAL自身超时，绝不在ISR或任务临界区里执行。 */
static void recover_receive(int id)
{
    uart_slot_t *slot = &s_slots[id];
    TickType_t now = xTaskGetTickCount();
    if (!slot->recovery_pending ||
        (TickType_t)(now - slot->last_retry) < pdMS_TO_TICKS(UART_RX_RETRY_MS)) return;
    slot->last_retry = now;
    int result = uart_rx_abort(slot->interface);
    taskENTER_CRITICAL();
    if (result == IF_OK) {
        slot->recovery_pending = 0U;
        start_receive(id);
        if (!slot->recovery_pending) count_add(&slot->diagnostics.recoveries, 1U);
    } else {
        count_add(&slot->diagnostics.recovery_failures, 1U);
    }
    taskEXIT_CRITICAL();
}

int uart_poll_rx(int id, uint8_t *buffer, uint16_t capacity)
{
    if (!slot_is_registered(id) || buffer == NULL || capacity < UART_RX_BUF_SIZE)
        return 0;
    recover_receive(id);

    uart_slot_t *slot = &s_slots[id];
    uart_rx_message_t message;
    int length = 0;
    taskENTER_CRITICAL();
    if (slot->gap_pending) {
        /* 连续性已丢失，保守丢弃整队列；重置与清标记原子化，防止吞掉新断流。 */
        count_add(&slot->diagnostics.dropped_blocks,
                  (uint32_t)uxQueueMessagesWaiting(slot->rx_queue));
        xQueueReset(slot->rx_queue);
        slot->gap_pending = 0U;
        length = UART_RX_GAP;
    } else if (xQueueReceive(slot->rx_queue, &message, 0U) == pdTRUE) {
        length = message.length;
    }
    taskEXIT_CRITICAL();
    if (length > 0) memcpy(buffer, message.data, (size_t)length);
    return length;
}

int uart_get_rx_diagnostics(int id, uart_rx_diagnostics_t *out)
{
    if (!slot_is_registered(id) || out == NULL) return 0;
    taskENTER_CRITICAL();
    *out = s_slots[id].diagnostics;
    out->recovery_pending = s_slots[id].recovery_pending;
    taskEXIT_CRITICAL();
    return 1;
}

int uart_send(int id, uint8_t *buffer, uint16_t length)
{
    if (!slot_is_registered(id) || buffer == NULL || length == 0U)
        return IF_ERR;
    return s_slots[id].interface->send(s_slots[id].interface, buffer, length);
}

/* 跟踪最后停止位，不把入队成功当作发送完成；每端口只允许一张在途票据。 */
int uart_send_tracked(int id, uint8_t *buffer, uint16_t length, uint32_t *ticket)
{
    if (!slot_is_registered(id)) return IF_ERR;
    return uart_hw_send_tracked(s_slots[id].interface, buffer, length, ticket);
}

int uart_tx_status(int id, uint32_t ticket, uint32_t *completed_ms)
{
    if (!slot_is_registered(id)) return IF_ERR;
    return uart_hw_tx_status(s_slots[id].interface, ticket, completed_ms);
}
