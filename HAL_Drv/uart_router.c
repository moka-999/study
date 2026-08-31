/*
 * UART字节路由：ISR复制接收块到队列，RK任务在任务上下文取走。
 * 本层不知道TCP、JSON或TLV命令；不解析帧，不转发PC业务。
 */
#include "uart_router.h"
#include "uart_hw.h"
#include "main.h"
#include "FreeRTOS.h"
#include "queue.h"

#include <string.h>

#define UART_SLOT_COUNT       2
#define UART_RX_QUEUE_LENGTH  8U

typedef struct {
    uint16_t length;
    uint8_t data[UART_RX_BUF_SIZE];
} uart_rx_message_t;

typedef struct {
    uart_if_t *interface;
    QueueHandle_t rx_queue;
} uart_slot_t;

/* 管理状态留在.bss由启动代码清零；不能假设NOLOAD DMA段内的指针为NULL。 */
static uart_slot_t s_slots[UART_SLOT_COUNT];
/* 仅DMA载荷进入原.uart_dma段；回调只复制本次实际收到的字节。 */
static uint8_t s_rx_buffers[UART_SLOT_COUNT][UART_RX_BUF_SIZE]
    __attribute__((section(".uart_dma")));

static int slot_is_registered(int id)
{
    return id >= 0 && id < UART_SLOT_COUNT &&
           s_slots[id].interface != NULL && s_slots[id].rx_queue != NULL;
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *uart, uint16_t size)
{
    if (uart == NULL || size == 0U || size > UART_RX_BUF_SIZE) {
        return;
    }

    for (int id = 0; id < UART_SLOT_COUNT; ++id) {
        if (!slot_is_registered(id) || s_slots[id].interface->handle != uart) {
            continue;
        }

        uart_rx_message_t message;
        message.length = size;
        memcpy(message.data, s_rx_buffers[id], size);
        BaseType_t higher_priority_task_woken = pdFALSE;
        /*
         * ISR不能等队列腾位置；满队列时本接收块被丢弃，不能视为可靠送达。
         * 当前保持丢块策略，依靠上层帧校验/超时恢复，不在ISR解释协议。
         */
        (void)xQueueSendFromISR(s_slots[id].rx_queue, &message,
                               &higher_priority_task_woken);
        (void)s_slots[id].interface->start_rx(s_slots[id].interface,
                                             s_rx_buffers[id],
                                             UART_RX_BUF_SIZE);
        portYIELD_FROM_ISR(higher_priority_task_woken);
        return;
    }
}

void uart_register(int id, uart_if_t *interface)
{
    if (id < 0 || id >= UART_SLOT_COUNT || interface == NULL ||
        interface->start_rx == NULL || interface->send == NULL ||
        s_slots[id].rx_queue != NULL) {
        return; /* 启动期一次注册；拒绝重复分配队列和无效接口。 */
    }

    QueueHandle_t queue = xQueueCreate(UART_RX_QUEUE_LENGTH, sizeof(uart_rx_message_t));
    if (queue == NULL) {
        return; /* 保持槽位未注册，启动/轮询均跳过它，避免空队列断言。 */
    }
    s_slots[id].interface = interface;
    s_slots[id].rx_queue = queue;
}

void uart_start_all(void)
{
    uart_tx_mutex_init();
    uart_tx_isr_attach();
    for (int id = 0; id < UART_SLOT_COUNT; ++id) {
        if (slot_is_registered(id)) {
            (void)s_slots[id].interface->start_rx(s_slots[id].interface,
                                                 s_rx_buffers[id],
                                                 UART_RX_BUF_SIZE);
        }
    }
}

int uart_poll_rx(int id, uint8_t *buffer, uint16_t capacity)
{
    if (!slot_is_registered(id) || buffer == NULL || capacity < UART_RX_BUF_SIZE) {
        return 0;
    }
    uart_rx_message_t message;
    if (xQueueReceive(s_slots[id].rx_queue, &message, 0) != pdTRUE) {
        return 0;
    }
    memcpy(buffer, message.data, message.length);
    return (int)message.length;
}

int uart_send(int id, uint8_t *buffer, uint16_t length)
{
    if (!slot_is_registered(id) || buffer == NULL || length == 0U) {
        return IF_ERR;
    }
    return s_slots[id].interface->send(s_slots[id].interface, buffer, length);
}
