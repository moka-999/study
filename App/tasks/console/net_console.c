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

/* 每路独立缓存收发字节，不保留pbuf。会话号和环形队列跨任务访问时受临界区保护。 */
typedef struct {
    uint8_t tx[CONSOLE_TX_SIZE], rx[CONSOLE_RX_SIZE];
    size_t tx_head, tx_count, rx_head, rx_count;
    uint32_t session, dropped, reported_drop;
    /* 以下字段仅由lwIP线程读写，慢客户端只消耗本路未确认字节配额。 */
    struct tcp_pcb *pcb;
    uint16_t unacked;
    uint32_t last_progress;
    uint8_t transmitting;
} console_client_t;

static console_client_t s_clients[NET_CONSOLE_CLIENT_COUNT];
static uint32_t s_next_session, s_last_schedule;
static uint8_t s_started, s_callback_pending;
static struct tcp_pcb *s_listener;
static ip_addr_t s_bound_ip;

static console_client_t *free_client(void)
{
    for (unsigned i = 0; i < NET_CONSOLE_CLIENT_COUNT; ++i)
        if (!s_clients[i].pcb) return &s_clients[i];
    return NULL;
}

/* 调用方持有临界区或运行于唯一分配会话号的lwIP线程；0表示无连接。 */
static console_client_t *client_by_session(uint32_t session)
{
    if (session) {
        for (unsigned i = 0; i < NET_CONSOLE_CLIENT_COUNT; ++i)
            if (s_clients[i].session == session) return &s_clients[i];
    }
    return NULL;
}

static void reset_channel(console_client_t *client, uint32_t session)
{
    taskENTER_CRITICAL();
    client->session = session;
    client->tx_head = client->tx_count = client->rx_head = client->rx_count = 0U;
    client->reported_drop = client->dropped;
    taskEXIT_CRITICAL();
    client->unacked = 0U;
    client->transmitting = 0U;
}

/* 立即回收本路调试TCP资源，8888业务和另一路调试连接继续运行。 */
static void close_client(console_client_t *client)
{
    struct tcp_pcb *pcb = client->pcb;
    client->pcb = NULL;
    reset_channel(client, 0U);
    tcp_arg(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_abort(pcb);
}

static void client_error(void *arg, err_t error)
{
    console_client_t *client = arg;
    (void)error;
    /* err回调中PCB已被lwIP释放。 */
    client->pcb = NULL;
    reset_channel(client, 0U);
}

static err_t client_receive(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t error)
{
    console_client_t *client = arg;
    if (!p) {
        close_client(client);
        return ERR_ABRT;
    }
    uint16_t length = p->tot_len;
    taskENTER_CRITICAL();
    int fits = error == ERR_OK && length <= CONSOLE_RX_SIZE - client->rx_count;
    if (fits) {
        size_t tail = (client->rx_head + client->rx_count) % CONSOLE_RX_SIZE;
        size_t first = CONSOLE_RX_SIZE - tail;
        if (first > length) first = length;
        (void)pbuf_copy_partial(p, client->rx + tail, (u16_t)first, 0U);
        (void)pbuf_copy_partial(p, client->rx, (u16_t)(length - first), (u16_t)first);
        client->rx_count += length;
    }
    taskEXIT_CRITICAL();
    pbuf_free(p);
    if (!fits) {
        /* 输入不能截断成另一条控制命令；洪泛时仅断开调试客户端。 */
        close_client(client);
        return ERR_ABRT;
    }
    tcp_recved(pcb, length);
    return ERR_OK;
}

static err_t client_sent(void *arg, struct tcp_pcb *pcb, u16_t length)
{
    console_client_t *client = arg;
    (void)pcb;
    client->unacked = (uint16_t)(client->unacked - length);
    client->last_progress = sys_now();
    return ERR_OK;
}

static err_t accept_client(void *arg, struct tcp_pcb *pcb, err_t error)
{
    (void)arg;
    /* lwIP分配失败也会调用accept，此时pcb为NULL，不能对它执行abort。 */
    if (error != ERR_OK || !pcb) return error != ERR_OK ? error : ERR_MEM;
    console_client_t *client = free_client();
    if (!client) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    client->pcb = pcb;
    /* 两槽均已接手才关闭监听，空出任一槽后由轮询重新监听。 */
    if (!free_client()) {
        (void)tcp_close(s_listener);
        s_listener = NULL;
    }
    do {
        ++s_next_session;
    } while (!s_next_session || client_by_session(s_next_session));
    reset_channel(client, s_next_session);
    client->last_progress = sys_now();
    tcp_arg(pcb, client);
    tcp_recv(pcb, client_receive);
    tcp_sent(pcb, client_sent);
    tcp_err(pcb, client_error);
    tcp_nagle_disable(pcb);
    return ERR_OK;
}

static void send_records(console_client_t *client)
{
    uint8_t record[CONSOLE_RECORD_SIZE];
    /* 每路限制未确认字节数，给正式业务保留网络堆余量。 */
    while (client->unacked < CONSOLE_TCP_BUDGET) {
        taskENTER_CRITICAL();
        uint32_t dropped = client->dropped;
        int notice = dropped != client->reported_drop;
        size_t length = 0U;
        if (!notice && client->tx_count) {
            length = (size_t)client->tx[client->tx_head] |
                     (size_t)client->tx[(client->tx_head + 1U) % CONSOLE_TX_SIZE] << 8;
            for (size_t i = 0U; i < length; ++i)
                record[i] = client->tx[(client->tx_head + RECORD_HEADER_SIZE + i) % CONSOLE_TX_SIZE];
        }
        taskEXIT_CRITICAL();
        if (notice) length = (size_t)snprintf((char *)record, sizeof(record),
            "\r\n[net console] droppedBytes=%lu\r\n", (unsigned long)dropped);
        if (!length || length > CONSOLE_TCP_BUDGET - client->unacked) break;
        if (tcp_write(client->pcb, record, (u16_t)length, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
        if (!client->unacked) client->last_progress = sys_now();
        client->unacked += (uint16_t)length;
        taskENTER_CRITICAL();
        if (notice) client->reported_drop = dropped;
        else {
            client->tx_head = (client->tx_head + RECORD_HEADER_SIZE + length) % CONSOLE_TX_SIZE;
            client->tx_count -= RECORD_HEADER_SIZE + length;
        }
        taskEXIT_CRITICAL();
    }
    (void)tcp_output(client->pcb);
}

static void network_poll(void *arg)
{
    (void)arg;
    int available = netif_is_up(&gnetif) && netif_is_link_up(&gnetif) &&
                    !ip_addr_isany(netif_ip_addr4(&gnetif));
    if (!available || !ip_addr_cmp(&s_bound_ip, netif_ip_addr4(&gnetif))) {
        for (unsigned i = 0; i < NET_CONSOLE_CLIENT_COUNT; ++i)
            if (s_clients[i].pcb) close_client(&s_clients[i]);
        if (s_listener) { (void)tcp_close(s_listener); s_listener = NULL; }
        ip_addr_copy(s_bound_ip, *netif_ip_addr4(&gnetif));
    }
    /* PCB池满时延后监听；不能让tcp_new为调试服务回收已有业务连接。
     * B1已启用MEMP_STATS，池的使用量只在lwIP线程内检查。 */
    if (available && !s_listener && free_client() &&
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
    for (unsigned i = 0; i < NET_CONSOLE_CLIENT_COUNT; ++i) {
        console_client_t *client = &s_clients[i];
        if (!client->pcb) continue;
        taskENTER_CRITICAL();
        int pending = client->tx_count != 0U;
        taskEXIT_CRITICAL();
        if (!client->transmitting) client->last_progress = sys_now();
        client->transmitting = client->unacked || pending;
        if (client->transmitting && sys_now() - client->last_progress >= CONSOLE_STALL_MS)
            close_client(client);
        else send_records(client);
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

uint32_t net_console_session(unsigned slot)
{
    taskENTER_CRITICAL();
    uint32_t session = slot < NET_CONSOLE_CLIENT_COUNT ? s_clients[slot].session : 0U;
    taskEXIT_CRITICAL();
    return session;
}

size_t net_console_read(uint32_t session, uint8_t *data, size_t capacity)
{
    size_t length = 0U;
    taskENTER_CRITICAL();
    console_client_t *client = client_by_session(session);
    if (client) {
        length = client->rx_count < capacity ? client->rx_count : capacity;
        for (size_t i = 0U; i < length; ++i)
            data[i] = client->rx[(client->rx_head + i) % CONSOLE_RX_SIZE];
        client->rx_head = (client->rx_head + length) % CONSOLE_RX_SIZE;
        client->rx_count -= length;
    }
    taskEXIT_CRITICAL();
    return length;
}

int net_console_write(uint32_t session, const void *data, size_t length, int drop)
{
    if (!data || !length || length > CONSOLE_RECORD_SIZE) return -1;
    taskENTER_CRITICAL();
    int result = -1;
    console_client_t *client = client_by_session(session);
    if (client) {
        result = 0;
        /* 普通日志留出一条控制回复空间，不能让status/切换确认被刷屏饿死。 */
        size_t reserve = drop ? CONSOLE_REPLY_RESERVE : 0U;
        if (length + RECORD_HEADER_SIZE + reserve <= CONSOLE_TX_SIZE - client->tx_count) {
            size_t tail = (client->tx_head + client->tx_count) % CONSOLE_TX_SIZE;
            client->tx[tail] = (uint8_t)length;
            client->tx[(tail + 1U) % CONSOLE_TX_SIZE] = (uint8_t)(length >> 8);
            for (size_t i = 0U; i < length; ++i)
                client->tx[(tail + RECORD_HEADER_SIZE + i) % CONSOLE_TX_SIZE] = ((const uint8_t *)data)[i];
            client->tx_count += length + RECORD_HEADER_SIZE;
            result = 1;
        } else if (drop) {
            client->dropped = length > UINT32_MAX - client->dropped ? UINT32_MAX : client->dropped + (uint32_t)length;
        }
    }
    taskEXIT_CRITICAL();
    return result;
}

uint32_t net_console_client_dropped_bytes(unsigned slot)
{
    taskENTER_CRITICAL();
    uint32_t dropped = slot < NET_CONSOLE_CLIENT_COUNT ? s_clients[slot].dropped : 0U;
    taskEXIT_CRITICAL();
    return dropped;
}

uint32_t net_console_dropped_bytes(void)
{
    uint32_t total = 0U;
    taskENTER_CRITICAL();
    for (unsigned i = 0; i < NET_CONSOLE_CLIENT_COUNT; ++i) {
        uint32_t dropped = s_clients[i].dropped;
        total = dropped > UINT32_MAX - total ? UINT32_MAX : total + dropped;
    }
    taskEXIT_CRITICAL();
    return total;
}
