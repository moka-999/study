/* 调试TCP独立于8888业务；raw API只在lwIP线程调用，不占用额外任务栈。 */
#include "net_console.h"
#include "board_network_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/sys.h"
#include "lwip/stats.h"
#include <stdio.h>
#include <string.h>

#define CONSOLE_TX_SIZE 4096U
#define CONSOLE_RX_SIZE 512U
#define CONSOLE_RECORD_SIZE 256U
#define CONSOLE_TCP_BUDGET 1024U
#define CONSOLE_POLL_MS 20U
#define CONSOLE_STALL_MS 3000U
#define RECORD_HEADER_SIZE 2U
#define CONSOLE_REPLY_RESERVE (CONSOLE_RECORD_SIZE + RECORD_HEADER_SIZE)

extern struct netif gnetif;

/* 两个环仅作任务间复制，不保留pbuf，避免占住以太网接收池。 */
static uint8_t s_tx[CONSOLE_TX_SIZE], s_rx[CONSOLE_RX_SIZE];
static size_t s_tx_head, s_tx_count, s_rx_head, s_rx_count;
static uint32_t s_session, s_next_session, s_dropped, s_reported_drop;
static uint8_t s_started, s_callback_pending;
static uint32_t s_last_schedule;

/* 以下字段只由lwIP线程读写。未ACK字节单独限额，不能吃满全局TCP_SND_BUF。 */
static struct tcp_pcb *s_listener, *s_client;
static ip_addr_t s_bound_ip;
static uint16_t s_unacked;
static uint32_t s_last_progress;
static uint8_t s_transmitting;

static void reset_channel(uint32_t session)
{
    taskENTER_CRITICAL();
    s_session = session;
    s_tx_head = s_tx_count = s_rx_head = s_rx_count = 0U;
    s_reported_drop = s_dropped;
    taskEXIT_CRITICAL();
    s_unacked = 0U;
    s_transmitting = 0U;
}

/* 关闭调试连接时不调用PC业务、关电或SLT取消。使用RST立即回收调试TCP资源。 */
static void close_client(void)
{
    struct tcp_pcb *client = s_client;
    s_client = NULL;
    reset_channel(0U);
    if (client) {
        tcp_arg(client, NULL);
        tcp_err(client, NULL);
        tcp_recv(client, NULL);
        tcp_sent(client, NULL);
        tcp_abort(client);
    }
}

static void client_error(void *arg, err_t error)
{
    (void)arg;
    (void)error;
    /* err回调中PCB已被lwIP释放，不能再次close/abort。 */
    s_client = NULL;
    reset_channel(0U);
}

static err_t client_receive(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t error)
{
    (void)arg;
    if (!p) {
        close_client();
        return ERR_ABRT;
    }
    uint16_t length = p->tot_len;
    taskENTER_CRITICAL();
    int fits = error == ERR_OK && length <= CONSOLE_RX_SIZE - s_rx_count;
    if (fits) {
        size_t tail = (s_rx_head + s_rx_count) % CONSOLE_RX_SIZE;
        size_t first = CONSOLE_RX_SIZE - tail;
        if (first > length) first = length;
        (void)pbuf_copy_partial(p, s_rx + tail, (u16_t)first, 0U);
        (void)pbuf_copy_partial(p, s_rx, (u16_t)(length - first), (u16_t)first);
        s_rx_count += length;
    }
    taskEXIT_CRITICAL();
    pbuf_free(p);
    if (!fits) {
        /* 输入不能截断成另一条控制命令；洪泛时仅断开调试客户端。 */
        close_client();
        return ERR_ABRT;
    }
    tcp_recved(pcb, length);
    return ERR_OK;
}

static err_t client_sent(void *arg, struct tcp_pcb *pcb, u16_t length)
{
    (void)arg;
    (void)pcb;
    s_unacked = (uint16_t)(s_unacked - length);
    s_last_progress = sys_now();
    return ERR_OK;
}

static err_t accept_client(void *arg, struct tcp_pcb *pcb, err_t error)
{
    (void)arg;
    /* lwIP分配失败也会调用accept，此时pcb为NULL，不能对它执行abort。 */
    if (error != ERR_OK || !pcb) return error != ERR_OK ? error : ERR_MEM;
    if (s_client) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    /* 接手后关闭监听；其它尚未完成握手的连接不能形成无人消费的接收队列。 */
    (void)tcp_close(s_listener);
    s_listener = NULL;
    s_client = pcb;
    if (++s_next_session == 0U) ++s_next_session;
    reset_channel(s_next_session);
    s_last_progress = sys_now();
    tcp_recv(pcb, client_receive);
    tcp_sent(pcb, client_sent);
    tcp_err(pcb, client_error);
    tcp_nagle_disable(pcb);
    return ERR_OK;
}

static void send_records(void)
{
    uint8_t record[CONSOLE_RECORD_SIZE];
    /* 每次最多提交1024字节，ACK之前也不再追加，正式业务保留网络堆余量。 */
    while (s_unacked < CONSOLE_TCP_BUDGET) {
        taskENTER_CRITICAL();
        uint32_t dropped = s_dropped;
        int notice = dropped != s_reported_drop;
        size_t length = 0U;
        if (!notice && s_tx_count) {
            length = (size_t)s_tx[s_tx_head] |
                     (size_t)s_tx[(s_tx_head + 1U) % CONSOLE_TX_SIZE] << 8;
            for (size_t i = 0U; i < length; ++i)
                record[i] = s_tx[(s_tx_head + RECORD_HEADER_SIZE + i) % CONSOLE_TX_SIZE];
        }
        taskEXIT_CRITICAL();
        if (notice) length = (size_t)snprintf((char *)record, sizeof(record),
            "\r\n[net console] droppedBytes=%lu\r\n", (unsigned long)dropped);
        if (!length || length > CONSOLE_TCP_BUDGET - s_unacked) break;
        if (tcp_write(s_client, record, (u16_t)length, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
        if (!s_unacked) s_last_progress = sys_now();
        s_unacked += (uint16_t)length;
        taskENTER_CRITICAL();
        if (notice) s_reported_drop = dropped;
        else {
            s_tx_head = (s_tx_head + RECORD_HEADER_SIZE + length) % CONSOLE_TX_SIZE;
            s_tx_count -= RECORD_HEADER_SIZE + length;
        }
        taskEXIT_CRITICAL();
    }
    (void)tcp_output(s_client);
}

static void network_poll(void *arg)
{
    (void)arg;
    int available = netif_is_up(&gnetif) && netif_is_link_up(&gnetif) &&
                    !ip_addr_isany(netif_ip_addr4(&gnetif));
    if (!available || !ip_addr_cmp(&s_bound_ip, netif_ip_addr4(&gnetif))) {
        close_client();
        if (s_listener) { (void)tcp_close(s_listener); s_listener = NULL; }
        ip_addr_copy(s_bound_ip, *netif_ip_addr4(&gnetif));
    }
    /* PCB池满时延后监听；不能让tcp_new为调试服务回收已有业务连接。
     * B1已启用MEMP_STATS，池的使用量只在lwIP线程内检查。 */
    if (available && !s_client && !s_listener &&
        lwip_stats.memp[MEMP_TCP_PCB]->used < lwip_stats.memp[MEMP_TCP_PCB]->avail) {
        struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (pcb) {
            tcp_setprio(pcb, TCP_PRIO_MIN);
            ip_set_option(pcb, SOF_REUSEADDR);
            if (tcp_bind(pcb, &s_bound_ip, B1_DEBUG_TCP_PORT) == ERR_OK)
                s_listener = tcp_listen_with_backlog(pcb, 1U);
            if (s_listener) tcp_accept(s_listener, accept_client);
            else tcp_abort(pcb);
        }
    }
    if (s_client) {
        taskENTER_CRITICAL();
        int pending = s_tx_count != 0U;
        taskEXIT_CRITICAL();
        if (!s_transmitting) s_last_progress = sys_now();
        s_transmitting = s_unacked || pending;
        if (s_transmitting && sys_now() - s_last_progress >= CONSOLE_STALL_MS)
            close_client();
        else send_records();
    }
    taskENTER_CRITICAL();
    s_callback_pending = 0U;
    taskEXIT_CRITICAL();
}

void net_console_start(void) { s_started = 1U; }

void net_console_process(void)
{
    if (!s_started || sys_now() - s_last_schedule < CONSOLE_POLL_MS) return;
    taskENTER_CRITICAL();
    int schedule = !s_callback_pending;
    if (schedule) s_callback_pending = 1U;
    taskEXIT_CRITICAL();
    if (!schedule) return;
    s_last_schedule = sys_now();
    if (tcpip_try_callback(network_poll, NULL) != ERR_OK) {
        taskENTER_CRITICAL();
        s_callback_pending = 0U;
        taskEXIT_CRITICAL();
    }
}

uint32_t net_console_session(void)
{
    taskENTER_CRITICAL();
    uint32_t session = s_session;
    taskEXIT_CRITICAL();
    return session;
}

size_t net_console_read(uint32_t session, uint8_t *data, size_t capacity)
{
    taskENTER_CRITICAL();
    size_t length = session && session == s_session ? s_rx_count : 0U;
    if (length > capacity) length = capacity;
    for (size_t i = 0U; i < length; ++i) data[i] = s_rx[(s_rx_head + i) % CONSOLE_RX_SIZE];
    s_rx_head = (s_rx_head + length) % CONSOLE_RX_SIZE;
    s_rx_count -= length;
    taskEXIT_CRITICAL();
    return length;
}

int net_console_write(uint32_t session, const void *data, size_t length, int drop)
{
    if (!data || !length || length > CONSOLE_RECORD_SIZE) return -1;
    taskENTER_CRITICAL();
    int result = -1;
    if (session && session == s_session) {
        result = 0;
        /* 普通日志留出一条控制回复空间，不能让status/切换确认被刷屏饿死。 */
        size_t reserve = drop ? CONSOLE_REPLY_RESERVE : 0U;
        if (length + RECORD_HEADER_SIZE + reserve <= CONSOLE_TX_SIZE - s_tx_count) {
            size_t tail = (s_tx_head + s_tx_count) % CONSOLE_TX_SIZE;
            s_tx[tail] = (uint8_t)length;
            s_tx[(tail + 1U) % CONSOLE_TX_SIZE] = (uint8_t)(length >> 8);
            for (size_t i = 0U; i < length; ++i)
                s_tx[(tail + RECORD_HEADER_SIZE + i) % CONSOLE_TX_SIZE] = ((const uint8_t *)data)[i];
            s_tx_count += length + RECORD_HEADER_SIZE;
            result = 1;
        } else if (drop) {
            s_dropped = length > UINT32_MAX - s_dropped ? UINT32_MAX : s_dropped + (uint32_t)length;
        }
    }
    taskEXIT_CRITICAL();
    return result;
}

uint32_t net_console_dropped_bytes(void) { return s_dropped; }
