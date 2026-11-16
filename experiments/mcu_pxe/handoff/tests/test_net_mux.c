/* 正式纯C分流模块：检查实际报文内容，不访问网卡、串口或上位机。 */
#include "net_mux.h"
#include <string.h>

#define STRING0(x) #x
#define STRING(x) STRING0(x)
#define CHECK(x) do { if (!(x)) fail("FAIL " STRING(__LINE__) " " #x "\n"); } while (0)
#define ETH_SIZE 14U
#define IP_SIZE 20U
#define PC_PORT 8888U
#define DEBUG_PORT 8889U
#define TCP_PROTOCOL 6U
#define UDP_PROTOCOL 17U
#define ICMP_PROTOCOL 1U

static const uint8_t physical[6] = {2U, 0xA8U, 0x9BU, 0x51U, 0x0FU, 0x1EU};
static const uint8_t virtual[6] = {6U, 0xA8U, 0x9BU, 0x51U, 0x0FU, 0x1EU};
static const uint8_t peer[6] = {0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U};
static const uint8_t ip[4] = {192U, 168U, 1U, 201U};
static const uint8_t peer_ip[4] = {192U, 168U, 1U, 200U};
static uint8_t frame[SN_ETH_MAX], original[SN_ETH_MAX];

static void message(const char *text)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = text;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}
static _Noreturn void finish(uint32_t code)
{
    uint32_t block[] = {0x20026U, code};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) {}
}
static _Noreturn void fail(const char *text) { message(text); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }

static void be16(uint8_t *p, unsigned value)
{
    p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
}
static uint16_t ipv4(unsigned protocol, unsigned payload, int transmit)
{
    memset(frame, 0, sizeof(frame));
    memcpy(frame, transmit ? peer : physical, 6U);
    memcpy(frame + 6U, transmit ? virtual : peer, 6U);
    be16(frame + 12U, 0x0800U);
    uint8_t *packet = frame + ETH_SIZE;
    packet[0] = 0x45U;
    be16(packet + 2U, IP_SIZE + payload);
    packet[8] = 64U; packet[9] = (uint8_t)protocol;
    memcpy(packet + 12U, transmit ? ip : peer_ip, 4U);
    memcpy(packet + 16U, transmit ? peer_ip : ip, 4U);
    return (uint16_t)(ETH_SIZE + IP_SIZE + payload);
}
static uint16_t tcp(unsigned source, unsigned destination, int transmit)
{
    uint16_t length = ipv4(TCP_PROTOCOL, 20U, transmit);
    uint8_t *transport = frame + ETH_SIZE + IP_SIZE;
    be16(transport, source); be16(transport + 2U, destination);
    transport[12] = 0x50U;
    transport[13] = 0x10U;
    return length;
}
static uint16_t udp(unsigned source, unsigned destination, unsigned payload, int transmit)
{
    uint16_t length = ipv4(UDP_PROTOCOL, 8U + payload, transmit);
    uint8_t *transport = frame + ETH_SIZE + IP_SIZE;
    be16(transport, source); be16(transport + 2U, destination);
    be16(transport + 4U, 8U + payload);
    for (unsigned i = 0U; i < payload; ++i) transport[8U + i] = (uint8_t)i;
    return length;
}
static uint16_t arp(unsigned operation, int transmit)
{
    memset(frame, 0, sizeof(frame));
    memcpy(frame, transmit ? peer : physical, 6U);
    memcpy(frame + 6U, transmit ? virtual : peer, 6U);
    be16(frame + 12U, 0x0806U);
    uint8_t *packet = frame + ETH_SIZE;
    be16(packet, 1U); be16(packet + 2U, 0x0800U);
    packet[4] = 6U; packet[5] = 4U;
    be16(packet + 6U, operation);
    memcpy(packet + 8U, transmit ? virtual : peer, 6U);
    memcpy(packet + 14U, transmit ? ip : peer_ip, 4U);
    memcpy(packet + 18U, transmit ? peer : physical, 6U);
    memcpy(packet + 24U, transmit ? peer_ip : ip, 4U);
    return ETH_SIZE + 28U;
}
static net_mux_rx_t receive(uint16_t length)
{
    return net_mux_rx(frame, length, physical, virtual, ip, PC_PORT, 8889U);
}
static net_mux_tx_t send(uint16_t length)
{
    return net_mux_tx(frame, length, physical, virtual, ip, PC_PORT, 8889U);
}

static void test_tcp_udp(void)
{
    uint16_t length = tcp(50000U, PC_PORT, 0);
    memcpy(original, frame, length);
    CHECK(receive(length) == NET_MUX_LOCAL && !memcmp(frame, original, length));
    length = tcp(50000U, DEBUG_PORT, 0);
    CHECK(receive(length) == NET_MUX_LOCAL);
    length = tcp(DEBUG_PORT, 50000U, 1);
    CHECK(send(length) == NET_MUX_INVALID);
    length = tcp(50000U, PC_PORT, 0);
    /* 旧ARP缓存仍发往虚拟MAC时也须识别MCU业务，集成层再修正原pbuf目的MAC。 */
    memcpy(frame, virtual, 6U);
    CHECK(receive(length) == NET_MUX_LOCAL);
    length = tcp(443U, 50000U, 0);
    memcpy(original, frame, length);
    CHECK(receive(length) == NET_MUX_RK && !memcmp(frame, virtual, 6U));
    CHECK(!memcmp(frame + ETH_SIZE, original + ETH_SIZE, length - ETH_SIZE));
    length = tcp(PC_PORT, 50000U, 1);
    CHECK(send(length) == NET_MUX_INVALID);
    length = tcp(50000U, 443U, 1);
    memcpy(original, frame, length);
    CHECK(send(length) == NET_MUX_SEND && !memcmp(frame + 6U, physical, 6U));
    CHECK(!memcmp(frame + ETH_SIZE, original + ETH_SIZE, length - ETH_SIZE));
    /* RRQ端口、服务端动态TID以及1468字节DATA均只给RK，不触发lwIP端口不可达。 */
    const unsigned ports[] = {69U, 40001U, 62301U};
    for (unsigned i = 0U; i < 3U; ++i) {
        length = udp(ports[i], 49152U, 1472U, 0);
        CHECK(length == SN_ETH_MAX);
        memcpy(original, frame, length);
        CHECK(receive(length) == NET_MUX_RK && !memcmp(frame, virtual, 6U));
        CHECK(!memcmp(frame + ETH_SIZE, original + ETH_SIZE, length - ETH_SIZE));
        length = udp(49152U, ports[i], 1028U, 1);
        memcpy(original, frame, length);
        CHECK(send(length) == NET_MUX_SEND && !memcmp(frame + 6U, physical, 6U));
        CHECK(!memcmp(frame + ETH_SIZE, original + ETH_SIZE, length - ETH_SIZE));
    }
    /* 混合控制ACK和下载包逐帧分发，无端口缓存/上一个报文遗留状态。 */
    for (unsigned i = 0U; i < 1000U; ++i) {
        length = tcp(50000U, PC_PORT, 0); CHECK(receive(length) == NET_MUX_LOCAL);
        length = udp(40001U, 49152U, 1472U, 0); CHECK(receive(length) == NET_MUX_RK);
    }
    message("PASS: same-IP TCP control and dynamic-port full-MTU TFTP split\n");
}

static void test_arp(void)
{
    uint16_t length = arp(1U, 0);
    memset(frame, 255, 6U);
    memcpy(original, frame, length);
    CHECK(receive(length) == NET_MUX_LOCAL && !memcmp(frame, original, length));
    length = arp(2U, 0);
    CHECK(receive(length) == NET_MUX_BOTH);
    CHECK(!memcmp(frame, virtual, 6U) && !memcmp(frame + ETH_SIZE + 18U, virtual, 6U));
    CHECK(!memcmp(frame + ETH_SIZE + 8U, peer, 6U));
    length = arp(1U, 1);
    CHECK(send(length) == NET_MUX_SEND);
    CHECK(!memcmp(frame + 6U, physical, 6U) && !memcmp(frame + ETH_SIZE + 8U, physical, 6U));
    length = arp(2U, 1);
    CHECK(send(length) == NET_MUX_SUPPRESS);
    length = arp(1U, 1);
    memcpy(frame + ETH_SIZE + 24U, ip, 4U);
    CHECK(send(length) == NET_MUX_SUPPRESS);
    memset(frame + ETH_SIZE + 14U, 0, 4U);
    CHECK(send(length) == NET_MUX_SUPPRESS);
    /* 外部冲突不得被MAC转换掩盖；本机反射包则不能骗EDK判重复地址。 */
    length = arp(1U, 0);
    memcpy(frame + ETH_SIZE + 14U, ip, 4U);
    CHECK(receive(length) == NET_MUX_BOTH);
    CHECK(!memcmp(frame + ETH_SIZE + 8U, peer, 6U));
    memcpy(frame + ETH_SIZE + 8U, physical, 6U);
    CHECK(receive(length) == NET_MUX_LOCAL);
    message("PASS: one external ARP owner, peer resolution and real conflict visibility\n");
}

static void test_dhcp(void)
{
    uint16_t length = udp(67U, 68U, 240U, 0);
    uint8_t *bootp = frame + ETH_SIZE + IP_SIZE + 8U;
    memset(bootp, 0, 240U);
    bootp[0] = 2U; bootp[1] = 1U; bootp[2] = 6U;
    memcpy(bootp + 16U, ip, 4U); memcpy(bootp + 28U, virtual, 6U);
    const uint8_t cookie[] = {99U, 130U, 83U, 99U};
    memcpy(bootp + 236U, cookie, 4U);
    memset(frame, 255, 6U); memset(frame + ETH_SIZE + 16U, 255, 4U);
    memcpy(original, frame, length);
    CHECK(receive(length) == NET_MUX_RK && !memcmp(frame, original, length));
    bootp[16U + 3U] ^= 1U;
    CHECK(receive(length) == NET_MUX_DROP);
    bootp[16U + 3U] ^= 1U; bootp[28U] ^= 1U;
    CHECK(receive(length) == NET_MUX_DROP);
    /* 外网源MAC改成02，但DHCP chaddr仍为06，沿用已有固定分配。 */
    length = udp(68U, 67U, 240U, 1);
    bootp = frame + ETH_SIZE + IP_SIZE + 8U;
    memset(bootp, 0, 240U);
    bootp[0] = 1U; bootp[1] = 1U; bootp[2] = 6U;
    memcpy(bootp + 28U, virtual, 6U); memcpy(bootp + 236U, cookie, 4U);
    memset(frame + ETH_SIZE + 12U, 0, 4U);
    memcpy(original, frame, length);
    CHECK(send(length) == NET_MUX_SEND && !memcmp(frame + 6U, physical, 6U));
    CHECK(!memcmp(frame + ETH_SIZE, original + ETH_SIZE, length - ETH_SIZE));
    message("PASS: DHCP chaddr binding and packet checksums stay unchanged\n");
}

static void test_icmp(void)
{
    uint16_t length = ipv4(ICMP_PROTOCOL, 8U, 0);
    uint8_t *icmp = frame + ETH_SIZE + IP_SIZE;
    icmp[0] = 8U; CHECK(receive(length) == NET_MUX_LOCAL);
    icmp[0] = 0U; CHECK(receive(length) == NET_MUX_RK);
    for (unsigned i = 0U; i < 3U; ++i) {
        length = ipv4(ICMP_PROTOCOL, 8U + IP_SIZE + 8U, 0);
        icmp = frame + ETH_SIZE + IP_SIZE;
        icmp[0] = 3U;
        uint8_t *quote = icmp + 8U;
        quote[0] = 0x45U; be16(quote + 2U, IP_SIZE + 20U);
        quote[9] = TCP_PROTOCOL; memcpy(quote + 12U, ip, 4U);
        be16(quote + IP_SIZE, i == 0U ? PC_PORT : i == 1U ? DEBUG_PORT : 50000U);
        CHECK(receive(length) == (i < 2U ? NET_MUX_LOCAL : NET_MUX_RK));
        quote[9] = UDP_PROTOCOL; CHECK(receive(length) == NET_MUX_RK);
        be16(quote + 6U, 1U); CHECK(receive(length) == NET_MUX_DROP);
    }
    message("PASS: ICMP echo ownership and quoted TCP/UDP error ownership\n");
}

static void test_boundaries(void)
{
    uint16_t length = tcp(50000U, PC_PORT, 0);
    for (uint16_t truncated = 0U; truncated < length; ++truncated)
        CHECK(receive(truncated) == NET_MUX_DROP);
    CHECK(net_mux_rx(NULL, length, physical, virtual, ip, PC_PORT, 8889U) == NET_MUX_DROP);
    CHECK(net_mux_tx(NULL, length, physical, virtual, ip, PC_PORT, 8889U) == NET_MUX_INVALID);
    frame[ETH_SIZE + IP_SIZE + 12U] = 0x40U;
    CHECK(receive(length) == NET_MUX_DROP);
    length = tcp(50000U, PC_PORT, 0);
    be16(frame + ETH_SIZE + 6U, 0x2000U);
    CHECK(receive(length) == NET_MUX_LOCAL);
    be16(frame + ETH_SIZE + 6U, 0x0010U);
    CHECK(receive(length) == NET_MUX_LOCAL);
    length = udp(40001U, 49152U, 16U, 0);
    be16(frame + ETH_SIZE + 6U, 0x2000U);
    CHECK(receive(length) == NET_MUX_DROP);
    length = udp(49152U, 40001U, 16U, 1);
    be16(frame + ETH_SIZE + 6U, 0x2000U);
    CHECK(send(length) == NET_MUX_INVALID);
    length = udp(40001U, 49152U, 16U, 0);
    frame[ETH_SIZE + 16U + 3U] = 202U;
    CHECK(receive(length) == NET_MUX_DROP);
    length = udp(40001U, 49152U, 16U, 0);
    be16(frame + ETH_SIZE + IP_SIZE + 4U, 7U);
    CHECK(receive(length) == NET_MUX_DROP);
    length = udp(49152U, 40001U, 16U, 1);
    be16(frame + ETH_SIZE + IP_SIZE + 4U, 7U);
    CHECK(send(length) == NET_MUX_INVALID);
    length = udp(40001U, 49152U, 16U, 0);
    be16(frame + 12U, 0x8100U); CHECK(receive(length) == NET_MUX_DROP);
    length = udp(49152U, 40001U, 16U, 1);
    frame[6U] = physical[0]; CHECK(send(length) == NET_MUX_INVALID);
    message("PASS: truncated/foreign/malformed frames and fragment boundaries\n");
}

void test_main(void)
{
    test_tcp_udp(); test_arp(); test_dhcp(); test_icmp(); test_boundaries();
    message("PASS: actual B1 same-IP net mux\n");
    finish(0U);
}
