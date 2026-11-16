/* 用实际lwIP处理SYN/ACK/数据/FIN/RST；网卡输出为内存，调度时钟由用例推进。 */
#include "net_console.h"
#include "board_network_config.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/ip4.h"
#include "lwip/inet_chksum.h"
#include "lwip/stats.h"
#include <string.h>

#define STR0(x) #x
#define STR(x) STR0(x)
#define CHECK(x) do { if (!(x)) test_assert("line " STR(__LINE__) ": " #x); } while (0)
#define IP_HEADER 20U
#define TCP_HEADER 20U
#define FLAG_FIN 1U
#define FLAG_SYN 2U
#define FLAG_RST 4U
#define FLAG_ACK 16U
struct netif gnetif;
static uint32_t now;
static unsigned critical, network_context;
static void (*queued_callback)(void *);
static void *queued_argument;
static int callback_fail;

void test_assert(const char *s);
static void message(const char *text)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = text;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}
static _Noreturn void finish(uint32_t code)
{
    const uint32_t block[] = {0x20026U, code};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register const uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) { }
}
void test_assert(const char *s) { message("FAIL: "); message(s); message("\n"); finish(1U); }
void test_abort(void) { test_assert("CPU exception"); }
void test_enter(void) { ++critical; }
void test_exit(void) { CHECK(critical); --critical; }
uint32_t sys_now(void) { return now; }
err_t tcpip_try_callback(void (*fn)(void *), void *arg)
{
    CHECK(!critical && !network_context && !queued_callback);
    if (callback_fail) return ERR_MEM;
    queued_callback = fn; queued_argument = arg;
    return ERR_OK;
}
static void poll_ms(uint32_t elapsed)
{
    now += elapsed;
    net_console_process();
    if (queued_callback) {
        network_context = 1U;
        void (*fn)(void *) = queued_callback;
        queued_callback = NULL;
        fn(queued_argument);
        network_context = 0U;
    }
    CHECK(!critical);
}

static unsigned read16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t read32(const uint8_t *p) { return (uint32_t)read16(p) << 16 | read16(p + 2); }
static void write16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void write32(uint8_t *p, uint32_t v) { write16(p, v >> 16); write16(p + 2, v); }

typedef struct {
    uint16_t port, service;
    uint32_t sequence, ack;
    unsigned resets, syns;
} peer_t;
static peer_t peers[] = {{50001U, B1_DEBUG_TCP_PORT, 100U, 0U, 0U, 0U},
                         {50002U, B1_DEBUG_TCP_PORT, 200U, 0U, 0U, 0U},
                         {50003U, B1_PC_TCP_PORT, 300U, 0U, 0U, 0U}};
static ip4_addr_t peer_ip;
static char output[32768];
static size_t output_length;
static unsigned business_received;

static err_t capture(struct netif *netif, struct pbuf *p, const ip4_addr_t *destination)
{
    (void)netif;
    (void)destination;
    CHECK(network_context && !critical);
    uint8_t packet[1600];
    CHECK(p->tot_len <= sizeof(packet));
    pbuf_copy_partial(p, packet, p->tot_len, 0U);
    unsigned ip_length = (packet[0] & 15U) * 4U;
    CHECK(packet[9] == 6U && inet_chksum(packet, (u16_t)ip_length) == 0U);
    uint8_t *tcp = packet + ip_length;
    peer_t *peer = NULL;
    for (unsigned i = 0U; i < sizeof(peers)/sizeof(peers[0]); ++i)
        if (peers[i].port == read16(tcp + 2)) peer = &peers[i];
    CHECK(peer);
    unsigned payload = read16(packet + 2) - ip_length - (tcp[12] >> 4) * 4U;
    if (tcp[13] & FLAG_RST) { ++peer->resets; return ERR_OK; }
    if (tcp[13] & FLAG_SYN) ++peer->syns;
    uint32_t next = read32(tcp + 4) + payload + ((tcp[13] & FLAG_SYN) != 0U) + ((tcp[13] & FLAG_FIN) != 0U);
    if (payload || tcp[13] & (FLAG_SYN | FLAG_FIN)) peer->ack = next;
    if (payload && peer->service == B1_DEBUG_TCP_PORT) {
        CHECK(output_length + payload < sizeof(output));
        memcpy(output + output_length, tcp + (tcp[12] >> 4) * 4U, payload);
        output_length += payload;
        output[output_length] = '\0';
    }
    return ERR_OK;
}
static err_t interface_init(struct netif *netif)
{
    netif->name[0] = 't'; netif->name[1] = 'c';
    netif->mtu = 1500U;
    netif->output = capture;
    return ERR_OK;
}

static void receive(peer_t *peer, uint8_t flags, const void *data, uint16_t length)
{
    uint8_t packet[1600] = {0};
    uint8_t *tcp = packet + IP_HEADER;
    unsigned total = IP_HEADER + TCP_HEADER + length;
    CHECK(total <= sizeof(packet));
    packet[0] = 0x45U; write16(packet + 2, total); packet[8] = 64U; packet[9] = 6U;
    memcpy(packet + 12, &peer_ip.addr, 4);
    memcpy(packet + 16, &gnetif.ip_addr.addr, 4);
    write16(tcp, peer->port); write16(tcp + 2, peer->service);
    write32(tcp + 4, peer->sequence); write32(tcp + 8, peer->ack);
    tcp[12] = 0x50U; tcp[13] = flags; write16(tcp + 14, 4096U);
    if (length) memcpy(tcp + TCP_HEADER, data, length);
    struct pbuf view = {0};
    view.payload = tcp; view.len = view.tot_len = TCP_HEADER + length;
    uint16_t sum = inet_chksum_pseudo(&view, 6U, view.len, &peer_ip, netif_ip4_addr(&gnetif));
    memcpy(tcp + 16, &sum, sizeof(sum));
    sum = inet_chksum(packet, IP_HEADER); memcpy(packet + 10, &sum, sizeof(sum));
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)total, PBUF_POOL);
    CHECK(p && pbuf_take(p, packet, (u16_t)total) == ERR_OK);
    network_context = 1U;
    CHECK(ip4_input(p, &gnetif) == ERR_OK);
    network_context = 0U;
    peer->sequence += length + ((flags & FLAG_SYN) != 0U) + ((flags & FLAG_FIN) != 0U);
}
static void connect_peer(peer_t *peer)
{
    unsigned previous = peer->syns;
    receive(peer, FLAG_SYN, NULL, 0U);
    CHECK(peer->syns == previous + 1U);
    receive(peer, FLAG_ACK, NULL, 0U);
}
static err_t business_receive(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t error)
{
    (void)arg;
    CHECK(p && error == ERR_OK);
    business_received += p->tot_len;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}
static err_t business_accept(void *arg, struct tcp_pcb *pcb, err_t error)
{
    (void)arg;
    CHECK(error == ERR_OK);
    tcp_recv(pcb, business_receive);
    return ERR_OK;
}
static void clear_output(void) { output_length = 0U; output[0] = '\0'; }

void test_main(void)
{
    lwip_init();
    ip4_addr_t ip, mask, gateway;
    IP4_ADDR(&ip,192,168,1,201); IP4_ADDR(&peer_ip,192,168,1,200);
    IP4_ADDR(&mask,255,255,255,0); IP4_ADDR(&gateway,0,0,0,0);
    CHECK(netif_add(&gnetif, &ip, &mask, &gateway, NULL, interface_init, ip4_input));
    netif_set_default(&gnetif); netif_set_up(&gnetif); netif_set_link_up(&gnetif);
    struct tcp_pcb *business = tcp_new();
    CHECK(business && tcp_bind(business, IP_ADDR_ANY, B1_PC_TCP_PORT) == ERR_OK);
    business = tcp_listen(business); CHECK(business); tcp_accept(business, business_accept);
    connect_peer(&peers[2]);

    net_console_start();
    callback_fail = 1; poll_ms(20U); CHECK(!net_console_session());
    struct tcp_pcb *held[MEMP_NUM_TCP_PCB - 1U];
    for (unsigned i = 0U; i < MEMP_NUM_TCP_PCB - 1U; ++i) { held[i] = tcp_new(); CHECK(held[i]); }
    callback_fail = 0; poll_ms(20U);
    CHECK(peers[2].resets == 0U);
    for (unsigned i = 0U; i < MEMP_NUM_TCP_PCB - 1U; ++i) tcp_abort(held[i]);
    poll_ms(20U);
    /* 无可用PCB时accept收到NULL/ERR_MEM，不能崩溃或牺牲已有业务连接。 */
    for (unsigned i = 0U; i < MEMP_NUM_TCP_PCB - 1U; ++i) { held[i] = tcp_new(); CHECK(held[i]); }
    receive(&peers[0], FLAG_SYN, NULL, 0U); CHECK(!net_console_session());
    for (unsigned i = 0U; i < MEMP_NUM_TCP_PCB - 1U; ++i) tcp_abort(held[i]);
    connect_peer(&peers[0]);
    uint32_t session = net_console_session(); CHECK(session);
    unsigned resets = peers[1].resets;
    receive(&peers[1], FLAG_SYN, NULL, 0U);
    CHECK(peers[1].resets == resets + 1U && net_console_session() == session);
    receive(&peers[0], FLAG_ACK, "sta", 3U);
    receive(&peers[0], FLAG_ACK, "tus\r\n", 5U);
    uint8_t input[512];
    CHECK(net_console_read(session, input, sizeof(input)) == 8U && !memcmp(input, "status\r\n", 8U));
    CHECK(lwip_stats.memp[MEMP_PBUF_POOL]->used == 0U);
    message("PASS: actual TCP handshake, duplicate refusal and immediate RX pbuf release\n");

    /* 长期空闲后仍可打印；2KiB日志只能先提交1KiB，ACK后再推进。 */
    poll_ms(10000U);
    char record[256]; memset(record, 'x', sizeof(record));
    for (unsigned i = 0U; i < 8U; ++i) CHECK(net_console_write(session, record, sizeof(record), 1) == 1);
    poll_ms(20U); CHECK(output_length == 1024U);
    poll_ms(20U); CHECK(output_length == 1024U && net_console_session() == session);
    receive(&peers[0], FLAG_ACK, NULL, 0U); poll_ms(20U); CHECK(output_length == 2048U);
    receive(&peers[0], FLAG_ACK, NULL, 0U); poll_ms(20U);
    clear_output();
    unsigned accepted = 0U;
    while (net_console_write(session, record, sizeof(record), 1) == 1) ++accepted;
    CHECK(accepted == 14U && net_console_dropped_bytes() == sizeof(record));
    CHECK(net_console_write(session, "control reply\r\n", 15U, 0) == 1);
    for (unsigned i = 0U; i < 10U; ++i) { poll_ms(20U); receive(&peers[0], FLAG_ACK, NULL, 0U); }
    CHECK(strstr(output, "droppedBytes=256") && strstr(output, "control reply\r\n"));
    message("PASS: bounded TCP backlog, whole-record drops and reserved control reply space\n");

    /* 不ACK的调试客户端会被释放，另一业务连接仍收包；旧会话数据不得重放。 */
    CHECK(net_console_write(session, "blocked", 7U, 1) == 1);
    poll_ms(20U); poll_ms(3000U); CHECK(!net_console_session());
    receive(&peers[2], FLAG_ACK, "business", 8U); CHECK(business_received == 8U);
    poll_ms(20U); connect_peer(&peers[0]); CHECK(net_console_session() != session);
    CHECK(net_console_write(session, "old", 3U, 0) == -1);
    CHECK(net_console_read(session, input, sizeof(input)) == 0U);
    session = net_console_session();
    receive(&peers[0], FLAG_ACK, record, 256U);
    receive(&peers[0], FLAG_ACK, record, 256U);
    receive(&peers[0], FLAG_ACK, "x", 1U); CHECK(!net_console_session());
    CHECK(net_console_read(session, input, sizeof(input)) == 0U);
    CHECK(lwip_stats.memp[MEMP_PBUF_POOL]->used == 0U);
    message("PASS: slow/input-flood clients are isolated from the live business connection\n");

    poll_ms(20U); connect_peer(&peers[0]);
    receive(&peers[0], FLAG_ACK | FLAG_FIN, NULL, 0U); CHECK(!net_console_session());
    poll_ms(20U); connect_peer(&peers[0]);
    receive(&peers[0], FLAG_ACK | FLAG_RST, NULL, 0U); CHECK(!net_console_session());
    poll_ms(20U); connect_peer(&peers[0]); session = net_console_session();
    IP4_ADDR(&ip,192,168,1,202);
    network_context = 1U; netif_set_ipaddr(&gnetif, &ip); network_context = 0U;
    poll_ms(20U);
    CHECK(!net_console_session());
    connect_peer(&peers[0]); CHECK(net_console_session() && net_console_session() != session);
    network_context = 1U; netif_set_link_down(&gnetif); network_context = 0U;
    poll_ms(20U); CHECK(!net_console_session());
    network_context = 1U; netif_set_link_up(&gnetif); network_context = 0U;
    poll_ms(20U); connect_peer(&peers[0]);
    CHECK(net_console_session());
    message("PASS: FIN/RST, dynamic IP and link recovery without stale connection output\n");
    CHECK(!critical);
    message("PASS: actual lwIP network console\n");
    finish(0U);
}
