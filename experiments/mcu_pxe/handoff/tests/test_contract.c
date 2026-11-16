/* 实际V2编解码及地址门禁离线测试；QEMU不模拟网络、PHY或RK启动。 */
#include "serial_net.h"
#include "ip_guard.h"
#include <string.h>

#define STR0(x) #x
#define STR(x) STR0(x)
#define CHECK(x) do { if (!(x)) fail("FAIL " STR(__LINE__) " " #x "\n"); } while (0)
static void message(const char *s)
{
    register unsigned r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = s;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}
static _Noreturn void finish(unsigned code)
{
    unsigned block[] = {0x20026U, code};
    register unsigned r0 __asm__("r0") = 0x20U;
    register unsigned *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) {}
}
static _Noreturn void fail(const char *s) { message(s); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }

static const uint8_t mac[] = {6U, 42U, 223U, 149U, 45U, 4U};
static const uint8_t ip[] = {192U, 168U, 1U, 201U};
static uint8_t frame[SN_ETH_MAX], wire[SN_WIRE_MAX];
static sn_frame_t request, response, decoded;
static sn_parser_t parser;
static sn_server_t server;
static unsigned executions;

static void put16(uint8_t *p, unsigned value)
{
    p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
}

static uint16_t packet(unsigned dhcp)
{
    memset(frame, 0, sizeof(frame));
    memset(frame, 255, 6U);
    memcpy(frame + 6U, mac, 6U);
    put16(frame + 12U, 0x0800U);
    uint8_t *p = frame + 14U;
    p[0] = 0x45U; p[8] = 64U; p[9] = 17U;
    unsigned size = dhcp ? 248U : 12U;
    put16(p + 2U, 20U + size);
    put16(p + 6U, 0x4000U);
    memcpy(p + 12U, ip, 4U);
    memset(p + 16U, 255, 4U);
    uint8_t *udp = p + 20U;
    put16(udp, dhcp ? 68U : 1234U);
    put16(udp + 2U, dhcp ? 67U : 69U);
    put16(udp + 4U, size);
    if (dhcp) {
        static const uint8_t cookie[] = {99U, 130U, 83U, 99U};
        uint8_t *body = udp + 8U;
        body[0] = 1U; body[1] = 1U; body[2] = 6U;
        memcpy(body + 28U, mac, 6U);
        memcpy(body + 236U, cookie, 4U);
    }
    return (uint16_t)(14U + 20U + size);
}

static void guards(void)
{
    uint16_t n = packet(0U);
    CHECK(sn_ip_guard_tx(frame, n, mac, ip));
    frame[29] = 200U;
    CHECK(!sn_ip_guard_tx(frame, n, mac, ip));
    CHECK(sn_ip_guard_rx(frame, n, mac, ip));
    memset(frame + 26U, 0, 4U);
    CHECK(!sn_ip_guard_tx(frame, n, mac, ip));
    n = packet(1U);
    memset(frame + 26U, 0, 4U);
    CHECK(sn_ip_guard_tx(frame, n, mac, ip));
    frame[42U + 28U] ^= 2U;
    CHECK(!sn_ip_guard_tx(frame, n, mac, ip));
    n = packet(1U);
    put16(frame + 34U, 67U); put16(frame + 36U, 68U);
    frame[42U] = 2U;
    memcpy(frame + 42U + 16U, ip, 4U);
    CHECK(sn_ip_guard_rx(frame, n, mac, ip));
    frame[42U + 19U] = 202U;
    CHECK(!sn_ip_guard_rx(frame, n, mac, ip));
    memset(frame + 42U + 16U, 0, 4U);
    CHECK(sn_ip_guard_rx(frame, n, mac, ip));
    frame[42U + 236U] = 0U;
    CHECK(!sn_ip_guard_rx(frame, n, mac, ip));
    n = packet(0U);
    put16(frame + 20U, 0x2000U);
    CHECK(!sn_ip_guard_tx(frame, n, mac, ip));
    CHECK(!sn_ip_guard_rx(frame, n, mac, ip));
    n = packet(0U);
    for (uint16_t short_n = 0U; short_n < n; ++short_n)
        CHECK(!sn_ip_guard_rx(frame, short_n, mac, ip));
    CHECK(!sn_ip_guard_tx(NULL, n, mac, ip));
    memset(frame, 0, sizeof(frame));
    memcpy(frame + 6U, mac, 6U); put16(frame + 12U, 0x0806U);
    uint8_t *arp = frame + 14U;
    put16(arp, 1U); put16(arp + 2U, 0x0800U);
    arp[4] = 6U; arp[5] = 4U; put16(arp + 6U, 1U);
    memcpy(arp + 8U, mac, 6U); memcpy(arp + 24U, ip, 4U);
    CHECK(sn_ip_guard_tx(frame, 42U, mac, ip));
    arp[27] = 202U;
    CHECK(!sn_ip_guard_tx(frame, 42U, mac, ip));
    memcpy(arp + 14U, ip, 4U);
    CHECK(sn_ip_guard_tx(frame, 42U, mac, ip));
    arp[8] ^= 2U;
    CHECK(!sn_ip_guard_tx(frame, 42U, mac, ip));
}

static void execute(void *context, const sn_frame_t *q, sn_frame_t *r)
{
    (void)context; (void)q;
    ++executions; r->status = SN_OK;
}

void test_main(void)
{
    guards();
    CHECK(SN_VERSION == 2U && SN_HELLO_INFO_SIZE == 21U);
    request.op = SN_HELLO; request.session = 42U; request.sequence = 0xffffffffU;
    CHECK(sn_server_handle(&server, &request, &response, execute, NULL) == 1);
    CHECK(sn_server_handle(&server, &request, &response, execute, NULL) == 0);
    request.session = 43U;
    CHECK(sn_server_handle(&server, &request, &response, execute, NULL) == -1);
    request.session = 42U; request.sequence = 0U; request.op = SN_RELEASE;
    CHECK(sn_server_handle(&server, &request, &response, execute, NULL) == 1);
    CHECK(sn_server_handle(&server, &request, &response, execute, NULL) == 0);
    CHECK(executions == 2U);
    uint16_t n = sn_pack(&response, wire, sizeof(wire));
    CHECK(n > 0U);
    for (uint16_t i = 0U; i < n; ++i) CHECK(sn_feed(&parser, wire[i], &decoded) == (i + 1U == n));
    CHECK(decoded.op == (SN_RELEASE | SN_REPLY) && decoded.session == 42U);
    request.sequence = 1U; request.op = SN_RELEASE + 1U;
    CHECK(sn_server_handle(&server, &request, &response, execute, NULL) == -1);
    request.sequence = 3U; request.op = SN_RELEASE;
    CHECK(sn_server_handle(&server, &request, &response, execute, NULL) == 1);
    CHECK(executions == 3U);
    request.sequence = 1U;
    CHECK(sn_server_handle(&server, &request, &response, execute, NULL) == -1);
    request.op = SN_SEND;
    CHECK(sn_server_handle(&server, &request, &response, execute, NULL) == -1);
    message("PASS: handoff contract and IPv4 guard\n");
    finish(0U);
}
