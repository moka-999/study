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

#define UART_SLOT_COUNT       UART_PORT_COUNT
#define UART_CONSOLE_QUEUE_LENGTH 2U
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
    uint8_t retry_delayed;
    TickType_t last_retry;
} uart_slot_t;

static uart_slot_t s_slots[UART_SLOT_COUNT];
static struct {
    uint8_t protocol[UART_RK_PROTOCOL_DMA_SIZE];
    uint8_t debug[UART_RX_BUF_SIZE];
    uint8_t console[UART_RX_BUF_SIZE];
} s_dma __attribute__((section(".uart_dma")));
static const struct {
    uint8_t *buffer;
    uint16_t size;
} s_rx_dma[UART_SLOT_COUNT] = {
    [UART_PORT_RK_PROTOCOL] = {s_dma.protocol, sizeof(s_dma.protocol)},
    [UART_PORT_RK_DEBUG] = {s_dma.debug, sizeof(s_dma.debug)},
    [UART_PORT_CONSOLE] = {s_dma.console, sizeof(s_dma.console)}
};
/* UART4软件环在注册时初始化。每一位记录对应字节前有断流，跨故障或改速时不会拼错帧。 */
static struct {
    uint8_t bytes[UART_RK_PROTOCOL_RX_CAPACITY];
    uint8_t gap_before[(UART_RK_PROTOCOL_RX_CAPACITY + 7U) / 8U];
    size_t head, count, gaps;
    uint16_t dma_pos;
    uint8_t dma_started;
} s_rk_rx __attribute__((section(".uart_rx_ring")));
/* USART2的单写者是同优先级的DMA/USART中断，唯一读者是默认任务。 */
static struct {
    uint8_t bytes[UART_RK_LOG_RX_CAPACITY];
    size_t head, count;
    uint16_t dma_pos;
} s_debug_rx;

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
    if (id == UART_PORT_RK_DEBUG) s_debug_rx.dma_pos = 0U;
    if (id == UART_PORT_RK_PROTOCOL) {
        s_rk_rx.dma_started = 0U;
        s_rk_rx.dma_pos = 0U;
    }
    if (slot->interface->start_rx(slot->interface, s_rx_dma[id].buffer,
                                  s_rx_dma[id].size) != IF_OK) {
        count_add(&slot->diagnostics.restart_failures, 1U);
        request_recovery(slot);
    } else if (id == UART_PORT_RK_PROTOCOL) s_rk_rx.dma_started = 1U;
}

static void record_protocol_drop(uart_slot_t *slot)
{
    count_add(&slot->diagnostics.dropped_blocks, 1U);
    slot->diagnostics.last_drop_ms = HAL_GetTick();
}

/* HAL故障后，尚未发布的DMA尾段可能含坏字节。只对确实移动的写指针记一次丢块。 */
static void discard_faulted_protocol_tail(uart_slot_t *slot)
{
    if (!s_rk_rx.dma_started) return;
    uint16_t end = uart_hw_rx_position(slot->interface, UART_RK_PROTOCOL_DMA_SIZE);
    if (end != s_rk_rx.dma_pos) {
        s_rk_rx.dma_pos = end;
        record_protocol_drop(slot);
    }
}

/* UART4单写者为同抢占级的DMA/UART中断；任务在临界区也可收取尚未触发IDLE的尾段。 */
static void collect_protocol_bytes(uart_slot_t *slot)
{
    uint16_t end = uart_hw_rx_position(slot->interface, UART_RK_PROTOCOL_DMA_SIZE);
    size_t from = s_rk_rx.dma_pos;
    size_t length = (end + UART_RK_PROTOCOL_DMA_SIZE - from) % UART_RK_PROTOCOL_DMA_SIZE;
    s_rk_rx.dma_pos = end;
    if (!length) return;
    size_t room = UART_RK_PROTOCOL_RX_CAPACITY - s_rk_rx.count;
    size_t accepted = length < room ? length : room;
    size_t tail = (s_rk_rx.head + s_rk_rx.count) % UART_RK_PROTOCOL_RX_CAPACITY;
    if (accepted && slot->gap_pending) {
        s_rk_rx.gap_before[tail / 8U] |= (uint8_t)(1U << (tail % 8U));
        ++s_rk_rx.gaps;
        slot->gap_pending = 0U;
    }
    s_rk_rx.count += accepted;
    for (size_t left = accepted; left; ) {
        size_t copy = UART_RK_PROTOCOL_DMA_SIZE - from;
        if (copy > UART_RK_PROTOCOL_RX_CAPACITY - tail) copy = UART_RK_PROTOCOL_RX_CAPACITY - tail;
        if (copy > left) copy = left;
        memcpy(s_rk_rx.bytes + tail, s_dma.protocol + from, copy);
        from = (from + copy) % UART_RK_PROTOCOL_DMA_SIZE;
        tail = (tail + copy) % UART_RK_PROTOCOL_RX_CAPACITY;
        left -= copy;
    }
    if (accepted < length) {
        count_add(&slot->diagnostics.queue_overflows, 1U);
        record_protocol_drop(slot);
        slot->gap_pending = 1U;
    }
}

/* 先取断点以前的有效字节，下一次poll交付GAP，随后才能取断点以后的字节。 */
static int read_protocol_bytes(uart_slot_t *slot, uint8_t *buffer, size_t capacity)
{
    if (!s_rk_rx.count) {
        if (!slot->gap_pending) return 0;
        slot->gap_pending = 0U;
        return UART_RX_GAP;
    }
    size_t head = s_rk_rx.head;
    size_t length = s_rk_rx.count < capacity ? s_rk_rx.count : capacity;
    if (s_rk_rx.gaps) {
        for (size_t i = 0U; i < length; ++i) {
            size_t pos = (head + i) % UART_RK_PROTOCOL_RX_CAPACITY;
            uint8_t mask = (uint8_t)(1U << (pos % 8U));
            if (!(s_rk_rx.gap_before[pos / 8U] & mask)) continue;
            if (i == 0U) {
                s_rk_rx.gap_before[pos / 8U] &= (uint8_t)~mask;
                --s_rk_rx.gaps;
                return UART_RX_GAP;
            }
            length = i;
            break;
        }
    }
    size_t first = UART_RK_PROTOCOL_RX_CAPACITY - head;
    if (first > length) first = length;
    memcpy(buffer, s_rk_rx.bytes + head, first);
    memcpy(buffer + first, s_rk_rx.bytes, length - first);
    s_rk_rx.head = (head + length) % UART_RK_PROTOCOL_RX_CAPACITY;
    s_rk_rx.count -= length;
    if (!s_rk_rx.count) {
        uint8_t ready;
        (void)xQueueReceive(slot->rx_queue, &ready, 0U);
    }
    return (int)length;
}

/* 将循环DMA的一段连续数据追加到软件环，调用方已检查总剩余空间。 */
static void append_debug_bytes(const uint8_t *data, size_t length)
{
    size_t tail = (s_debug_rx.head + s_debug_rx.count) % UART_RK_LOG_RX_CAPACITY;
    size_t first = UART_RK_LOG_RX_CAPACITY - tail;
    if (first > length) first = length;
    memcpy(s_debug_rx.bytes + tail, data, first);
    memcpy(s_debug_rx.bytes, data + first, length - first);
    s_debug_rx.count += length;
}

/* HAL给出循环DMA的写入位置。HT、TC、IDLE可报告同一位置，重复通知不复制数据。 */
static void receive_debug_bytes(uart_slot_t *slot, uint16_t position, BaseType_t *wake)
{
    size_t from = s_debug_rx.dma_pos;
    size_t end = position % UART_RX_BUF_SIZE;
    size_t length = (end + UART_RX_BUF_SIZE - from) % UART_RX_BUF_SIZE;
    s_debug_rx.dma_pos = (uint16_t)end;
    if (!length) return;

    /* 在消费者看到GAP前不拼接断点两端；已经收到的前缀仍可正常交付。 */
    if (slot->gap_pending) {
        count_add(&slot->diagnostics.dropped_blocks, 1U);
        return;
    }
    size_t accepted = UART_RK_LOG_RX_CAPACITY - s_debug_rx.count;
    if (accepted < length) {
        count_add(&slot->diagnostics.queue_overflows, 1U);
        count_add(&slot->diagnostics.dropped_blocks, 1U);
        slot->gap_pending = 1U;
    } else accepted = length;
    if (!accepted) return;

    if (!s_debug_rx.count) {
        /* 单槽只表示“有字节可读”，使uart_wait_rx保留等待且不消耗数据的语义。 */
        const uint8_t ready = 1U;
        (void)xQueueSendFromISR(slot->rx_queue, &ready, wake);
    }
    const uint8_t *dma = s_dma.debug;
    size_t first = UART_RX_BUF_SIZE - from;
    if (first > accepted) first = accepted;
    append_debug_bytes(dma + from, first);
    if (accepted > first) append_debug_bytes(dma, accepted - first);
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *uart, uint16_t size)
{
    if (uart == NULL) return;

    for (int id = 0; id < UART_SLOT_COUNT; ++id) {
        if (!slot_is_registered(id) || s_slots[id].interface->handle != uart)
            continue;
        uart_slot_t *slot = &s_slots[id];
        if (id == UART_PORT_CONSOLE && HAL_UARTEx_GetRxEventType(uart) == HAL_UART_RXEVENT_HT)
            return; /* NORMAL模式忽略HT，IDLE/TC交付完整块。 */
        if ((size == 0U && id == UART_PORT_CONSOLE) || size > s_rx_dma[id].size) {
            count_add(&slot->diagnostics.invalid_events, 1U);
            request_recovery(slot);
            return;
        }
        if (slot->recovery_pending) {
            if (id == UART_PORT_RK_PROTOCOL) discard_faulted_protocol_tail(slot);
            else {
                count_add(&slot->diagnostics.dropped_blocks, 1U);
                slot->gap_pending = 1U;
            }
            return; /* 不把故障期间收到的块交给组帧，也不在ISR执行阻塞Abort。 */
        }

        BaseType_t higher_priority_task_woken = pdFALSE;
        if (id == UART_PORT_RK_PROTOCOL) {
            size_t before = s_rk_rx.count;
            collect_protocol_bytes(slot);
            if (!before && s_rk_rx.count) {
                const uint8_t ready = 1U;
                (void)xQueueSendFromISR(slot->rx_queue, &ready, &higher_priority_task_woken);
            }
            portYIELD_FROM_ISR(higher_priority_task_woken);
            return;
        }
        if (id == UART_PORT_RK_DEBUG) {
            receive_debug_bytes(slot, size, &higher_priority_task_woken);
            portYIELD_FROM_ISR(higher_priority_task_woken);
            return; /* 循环DMA保持接收，不在块与块之间重新武装。 */
        }
        uart_rx_message_t message;
        message.length = size;
        memcpy(message.data, s_dma.console, size);
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
        if (id == UART_PORT_RK_PROTOCOL) discard_faulted_protocol_tail(slot);
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

    QueueHandle_t queue;
    if (id != UART_PORT_CONSOLE) {
        queue = xQueueCreate(1U, sizeof(uint8_t));
    } else {
        queue = xQueueCreate(UART_CONSOLE_QUEUE_LENGTH, sizeof(uart_rx_message_t));
    }
    if (queue == NULL) return; /* 分配失败保持未注册；无空队列解引用。 */
    if (id == UART_PORT_RK_PROTOCOL) {
        /* AXI中的NOLOAD区复位后可能保留旧游标和断点，首次启DMA前清零。 */
        memset(&s_rk_rx, 0, sizeof(s_rk_rx));
    }
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

/* RK两路的新故障在下次轮询立即恢复，恢复失败才限速；控制台沿用100ms重试间隔。
 * Abort只停止RX，可能使用HAL自身超时，绝不在ISR或任务临界区里执行。 */
static void recover_receive(int id)
{
    uart_slot_t *slot = &s_slots[id];
    TickType_t now = xTaskGetTickCount();
    if (!slot->recovery_pending) return;
    if ((id == UART_PORT_CONSOLE || slot->retry_delayed) &&
        (TickType_t)(now - slot->last_retry) < pdMS_TO_TICKS(UART_RX_RETRY_MS)) return;
    slot->last_retry = now;
    int result = uart_rx_abort(slot->interface);
    taskENTER_CRITICAL();
    if (result == IF_OK) {
        if (id == UART_PORT_RK_PROTOCOL) discard_faulted_protocol_tail(slot);
        slot->recovery_pending = 0U;
        start_receive(id);
        if (!slot->recovery_pending) count_add(&slot->diagnostics.recoveries, 1U);
    } else {
        count_add(&slot->diagnostics.recovery_failures, 1U);
    }
    slot->retry_delayed = slot->recovery_pending;
    taskEXIT_CRITICAL();
}

/* 调用时已屏蔽接收中断。最后一个有效字节先于GAP交付给日志和DDR容量解析器。 */
static int read_debug_bytes(uart_slot_t *slot, uint8_t *buffer, size_t capacity)
{
    size_t length = s_debug_rx.count;
    if (length == 0U) {
        if (!slot->gap_pending) return 0;
        slot->gap_pending = 0U;
        return UART_RX_GAP;
    }
    if (length > capacity) length = capacity;
    size_t first = UART_RK_LOG_RX_CAPACITY - s_debug_rx.head;
    if (first > length) first = length;
    memcpy(buffer, s_debug_rx.bytes + s_debug_rx.head, first);
    memcpy(buffer + first, s_debug_rx.bytes, length - first);
    s_debug_rx.head = (s_debug_rx.head + length) % UART_RK_LOG_RX_CAPACITY;
    s_debug_rx.count -= length;
    if (!s_debug_rx.count) {
        uint8_t ready;
        (void)xQueueReceive(slot->rx_queue, &ready, 0U);
    }
    return (int)length;
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
    if (id == UART_PORT_RK_PROTOCOL) {
        length = read_protocol_bytes(slot, buffer, capacity);
        taskEXIT_CRITICAL();
        return length;
    }
    if (id == UART_PORT_RK_DEBUG) {
        length = read_debug_bytes(slot, buffer, capacity);
        taskEXIT_CRITICAL();
        return length;
    }
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

void uart_wait_rx(int id, uint32_t timeout_ms)
{
    if (!slot_is_registered(id) || s_slots[id].gap_pending) return;
    if (id == UART_PORT_RK_PROTOCOL) {
        uint8_t ready;
        taskENTER_CRITICAL();
        int readable = s_rk_rx.count != 0U;
        taskEXIT_CRITICAL();
        if (!readable) (void)xQueuePeek(s_slots[id].rx_queue, &ready, pdMS_TO_TICKS(timeout_ms));
        return;
    }
    /* 复用RX队列的ISR唤醒；peek保留原块，避免另建通知通道或抢走消费者的数据。 */
    uart_rx_message_t pending;
    (void)xQueuePeek(s_slots[id].rx_queue, &pending, pdMS_TO_TICKS(timeout_ms));
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

int uart_tx_idle(int id)
{
    return slot_is_registered(id) && uart_hw_tx_idle(s_slots[id].interface);
}

int uart_rk_set_baud(uint32_t baud, uint16_t *old_rx_bytes)
{
    const int id = UART_PORT_RK_PROTOCOL;
    if (!slot_is_registered(id) || !uart_tx_idle(id))
        return IF_ERR;
    uart_slot_t *slot = &s_slots[id];
    uart_baud_result_t change = uart_hw_set_baud(slot->interface, baud);
    if (change == UART_BAUD_UNCHANGED) return IF_ERR;
    taskENTER_CRITICAL();
    if (change == UART_BAUD_RX_FAULT) {
        request_recovery(slot);
        taskEXIT_CRITICAL();
        return IF_ERR;
    }
    /* 已停止的DMA保留NDTR。旧速尾部留给消费者，速率边界隔开前后两个解析流。 */
    if (slot->recovery_pending) discard_faulted_protocol_tail(slot);
    else collect_protocol_bytes(slot);
    slot->gap_pending = 1U;
    slot->recovery_pending = 0U;
    start_receive(id);
    int result = change == UART_BAUD_CHANGED && !slot->recovery_pending ? IF_OK : IF_ERR;
    if (result == IF_OK && old_rx_bytes) {
        /* 仅含边界前已缓存的有效字节；中断重新开放后的高速字节不计入。 */
        *old_rx_bytes = (uint16_t)s_rk_rx.count;
    }
    taskEXIT_CRITICAL();
    return result;
}

int uart_set_baud(int id, uint32_t baud)
{
    return id == UART_PORT_RK_PROTOCOL ? uart_rk_set_baud(baud, NULL) : IF_ERR;
}

int uart_rk_end_rx_session(void)
{
    if (!slot_is_registered(UART_PORT_RK_PROTOCOL)) return IF_ERR;
    uart_slot_t *slot = &s_slots[UART_PORT_RK_PROTOCOL];
    recover_receive(UART_PORT_RK_PROTOCOL);
    taskENTER_CRITICAL();
    if (slot->recovery_pending) {
        taskEXIT_CRITICAL();
        return IF_ERR;
    }
    /* RK已OFF，无新会话数据。记录旧会话剩余量并越过DMA尾部，保持115200连续接收。 */
    uint16_t pos = uart_hw_rx_position(slot->interface, UART_RK_PROTOCOL_DMA_SIZE);
    size_t tail = (pos + UART_RK_PROTOCOL_DMA_SIZE - s_rk_rx.dma_pos) % UART_RK_PROTOCOL_DMA_SIZE;
    count_add(&slot->diagnostics.session_end_bytes, (uint32_t)(s_rk_rx.count + tail));
    s_rk_rx.dma_pos = pos;
    s_rk_rx.head = s_rk_rx.count = s_rk_rx.gaps = 0U;
    memset(s_rk_rx.gap_before, 0, sizeof(s_rk_rx.gap_before));
    slot->gap_pending = 0U;
    xQueueReset(slot->rx_queue);
    taskEXIT_CRITICAL();
    return IF_OK;
}
