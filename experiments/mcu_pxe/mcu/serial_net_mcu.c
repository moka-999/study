#include "serial_net_mcu.h"
#include "serial_net.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "lwip/tcpip.h"
#include "main.h"
#include "uart_router.h"
#include "pwr_ctrl.h"
#include "task_watchdog.h"
#include "log_console.h"
#include <string.h>
#include <stdio.h>

typedef struct { uint16_t length; uint8_t data[SN_ETH_MAX]; } eth_packet_t;
static QueueHandle_t s_rx_queue;
static SemaphoreHandle_t s_tx_done;
static struct netif *s_netif;
static uint8_t s_mac[6];
static volatile uint8_t s_active;
static volatile uint8_t s_tx_pending;
static volatile err_t s_tx_result;
static eth_packet_t s_tx;
/* RX只被以太网线程调用，静态中转避免消耗其2KiB栈。 */
static eth_packet_t s_rx;
static uint32_t s_rx_count, s_tx_count, s_drops, s_bad;
/* 以太网包数与串口字节数分开，避免握手失败时把rx=0误判为UART无输入。 */
static uint32_t s_uart_bytes, s_frames, s_hello, s_tx_queued, s_tx_bytes, s_tx_fail, s_off_frames;
static sn_server_t s_server;
static sn_parser_t s_parser;
static sn_frame_t s_request, s_response;
static uint8_t s_uart_rx[UART_RX_BUF_SIZE], s_wire[SN_WIRE_MAX];

int serial_net_prepare(void)
{
    s_rx_queue = xQueueCreate(8U, sizeof(eth_packet_t));
    s_tx_done = xSemaphoreCreateBinary();
    return s_rx_queue != NULL && s_tx_done != NULL;
}

void serial_net_attach(struct netif *netif)
{
    memcpy(s_mac, netif->hwaddr, sizeof(s_mac));
    /* 板卡地址首字节固定02；06仍是本地单播，避免与MCU自身MAC重复。 */
    s_mac[0] = 0x06;
    ETH->MACA1LR = (uint32_t)s_mac[0] | (uint32_t)s_mac[1] << 8 |
                   (uint32_t)s_mac[2] << 16 | (uint32_t)s_mac[3] << 24;
    /* AE=1、SA=0：匹配目的MAC，HAL的SourceMACAddrMatch不能用于此处。 */
    ETH->MACA1HR = ETH_MACA1HR_AE | (uint32_t)s_mac[4] | (uint32_t)s_mac[5] << 8;
    /* PM仅放行组播，保留其它过滤位；IPv4组播范围由收包路径继续限制。 */
    ETH->MACPFR |= ETH_MACPFR_PM;
    s_netif = netif;
}

int serial_net_input(struct pbuf *p)
{
    uint8_t header[14];
    static const uint8_t broadcast[6] = {255,255,255,255,255,255};
    int ours, bcast, mcast;
    if (!s_netif || p->tot_len < sizeof(header)) return 0;
    if (pbuf_copy_partial(p, header, sizeof(header), 0) != sizeof(header)) return 0;
    ours = memcmp(header, s_mac, 6) == 0;
    bcast = memcmp(header, broadcast, 6) == 0;
    mcast = header[0] == 1U && header[1] == 0U && header[2] == 0x5EU && header[3] < 0x80U;
    if (!ours && !bcast && !mcast) return 0;
    if (!s_active || pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) return ours;
    /* 单播/广播保留IPv4和ARP；01:00:5e:00..7f组播只转IPv4，仍留给MCU的lwIP。 */
    if (header[12] != 8U || (header[13] != 0U && (header[13] != 6U || mcast))) return ours;
    if (p->tot_len > SN_ETH_MAX) { ++s_drops; return ours; }
    s_rx.length = p->tot_len;
    if (pbuf_copy_partial(p, s_rx.data, p->tot_len, 0) != p->tot_len ||
        xQueueSend(s_rx_queue, &s_rx, 0) != pdTRUE) ++s_drops;
    else ++s_rx_count;
    return ours;
}

/* 只在tcpip线程发送，复用现有ETH DMA引用计数，禁止跨任务直调HAL发送。 */
static void transmit_callback(void *unused)
{
    struct pbuf *p = NULL;
    err_t result = ERR_IF;
    (void)unused;
    if (s_netif && netif_is_link_up(s_netif) &&
        pwr_ctrl_get_state() == PWR_CTRL_STATE_ON) {
        p = pbuf_alloc(PBUF_RAW, s_tx.length, PBUF_RAM);
        result = p ? pbuf_take(p, s_tx.data, s_tx.length) : ERR_MEM;
        if (result == ERR_OK) result = s_netif->linkoutput(s_netif, p);
        if (p) pbuf_free(p);
    }
    taskENTER_CRITICAL();
    s_tx_result = result;
    (void)xSemaphoreGive(s_tx_done);
    s_tx_pending = 0U;
    taskEXIT_CRITICAL();
}

static uint8_t transmit(const sn_frame_t *q)
{
    if (q->length < 14U || memcmp(q->data + 6, s_mac, 6) ||
        q->data[12] != 8U || (q->data[13] != 0U && q->data[13] != 6U)) return SN_INVALID;
    if (!s_netif || !netif_is_link_up(s_netif) || !s_active) return SN_NOT_READY;
    if (s_tx_pending) return SN_NOT_READY;
    (void)xSemaphoreTake(s_tx_done, 0);
    memcpy(s_tx.data, q->data, q->length); s_tx.length = q->length;
    s_tx_pending = 1U;
    if (tcpip_callback(transmit_callback, NULL) != ERR_OK) {
        s_tx_pending = 0U; return SN_IO_ERROR;
    }
    /* 超时后缓冲仍归callback所有，下一请求不得覆写；不自动重发到网线。 */
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(2500U)) != pdTRUE) return SN_IO_ERROR;
    if (s_tx_result != ERR_OK) return SN_IO_ERROR;
    ++s_tx_count;
    return SN_OK;
}

static void execute(void *unused, const sn_frame_t *q, sn_frame_t *r)
{
    static eth_packet_t packet;
    (void)unused;
    r->status = SN_INVALID;
    if (q->op != SN_SEND && q->length) return;
    if (q->op == SN_HELLO || q->op == SN_RESET || q->op == SN_STOP) {
        s_active = 0U;
        xQueueReset(s_rx_queue);
        s_active = q->op != SN_STOP;
        r->status = SN_OK;
        if (q->op == SN_HELLO) {
            if (!s_netif) { s_active = 0U; r->status = SN_NOT_READY; return; }
            memcpy(r->data, s_mac, 6);
            r->data[6] = netif_is_link_up(s_netif) ? 1U : 0U;
            r->data[7] = (uint8_t)SN_MTU; r->data[8] = (uint8_t)(SN_MTU >> 8);
            r->length = 9U;
        }
    } else if (q->op == SN_SEND) r->status = transmit(q);
    else if (q->op == SN_POLL) {
        r->status = s_active && s_netif && netif_is_link_up(s_netif) ? SN_EMPTY : SN_NOT_READY;
        if (r->status == SN_EMPTY && xQueueReceive(s_rx_queue, &packet, 0) == pdTRUE) {
            r->length = packet.length; memcpy(r->data, packet.data, packet.length);
            r->status = SN_OK;
        }
    }
}

void StartSerialNetTask(void *argument)
{
    uint32_t last_byte = 0, last_report = 0;
    uint8_t was_on = 0;
    (void)argument;
    for (;;) {
        uint32_t now = HAL_GetTick();
        int on = pwr_ctrl_get_state() == PWR_CTRL_STATE_ON;
        if (!on && was_on) {
            s_active = 0U; s_server.valid = 0U;
            xQueueReset(s_rx_queue); sn_parser_reset(&s_parser);
        }
        was_on = (uint8_t)on;
        if ((uint32_t)(now - last_byte) > 1000U) sn_parser_reset(&s_parser);
        int count = uart_poll_rx(UART_PORT_RK_PROTOCOL, s_uart_rx, sizeof(s_uart_rx));
        if (count < 0) { sn_parser_reset(&s_parser); ++s_bad; }
        if (count > 0) {
            last_byte = now;
            s_uart_bytes += (uint32_t)count;
            for (int i = 0; i < count; ++i) {
                int result = sn_feed(&s_parser, s_uart_rx[i], &s_request);
                if (result < 0) ++s_bad;
                if (result == 1) {
                    ++s_frames;
                    if (s_request.op == SN_HELLO) ++s_hello;
                    if (!on) { ++s_off_frames; continue; }
                    (void)sn_server_handle(&s_server, &s_request, &s_response, execute, NULL);
                    uint16_t size = sn_pack(&s_response, s_wire, sizeof(s_wire));
                    int queued = size && uart_send(UART_PORT_RK_PROTOCOL, s_wire, size) == IF_OK;
                    if (queued) { ++s_tx_queued; s_tx_bytes += size; }
                    else ++s_tx_fail;
                    if (s_request.op == SN_HELLO) {
                        char line[144];
                        int length = snprintf(line, sizeof(line),
                            "[serial-net] HELLO session=%lu seq=%lu status=%u queued=%u\r\n",
                            (unsigned long)s_request.session, (unsigned long)s_request.sequence,
                            (unsigned)s_response.status, (unsigned)queued);
                        if (length > 0 && length < (int)sizeof(line))
                            (void)log_console_write(line, (size_t)length);
                    }
                }
            }
        }
        if (now - last_report >= 5000U) {
            char line[224]; last_report = now;
            int size = snprintf(line, sizeof(line), "[serial-net] diag=4 on=%u active=%u ethRx=%lu ethTx=%lu drop=%lu bad=%lu heap=%lu\r\n",
                (unsigned)on, (unsigned)s_active, (unsigned long)s_rx_count,
                (unsigned long)s_tx_count, (unsigned long)s_drops, (unsigned long)s_bad,
                (unsigned long)xPortGetFreeHeapSize());
            if (size > 0 && size < (int)sizeof(line)) (void)log_console_write(line, (size_t)size);
            /* queued仅表示进入TX环，不等于线路发送完成或RK收到。 */
            size = snprintf(line, sizeof(line),
                "[serial-net-uart] rxBytes=%lu frames=%lu hello=%lu txQueued=%lu txBytes=%lu txFail=%lu off=%lu partial=%u\r\n",
                (unsigned long)s_uart_bytes, (unsigned long)s_frames, (unsigned long)s_hello,
                (unsigned long)s_tx_queued, (unsigned long)s_tx_bytes, (unsigned long)s_tx_fail,
                (unsigned long)s_off_frames, (unsigned)s_parser.used);
            if (size > 0 && size < (int)sizeof(line)) (void)log_console_write(line, (size_t)size);
        }
        task_watchdog_progress(TASK_WATCHDOG_RK);
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}
