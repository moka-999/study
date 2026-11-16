#include "net_mux.h"
#include "ip_guard.h"
#include <string.h>

#define MAC_SIZE 6U
#define ETH_HEADER_SIZE 14U
#define ETH_TYPE_OFFSET 12U
#define ETH_IPV4 0x0800U
#define ETH_ARP 0x0806U
#define ARP_SIZE 28U
#define ARP_OPERATION_OFFSET 6U
#define ARP_SENDER_MAC_OFFSET 8U
#define ARP_SENDER_IP_OFFSET 14U
#define ARP_TARGET_MAC_OFFSET 18U
#define ARP_TARGET_IP_OFFSET 24U
#define ARP_REQUEST 1U
#define ARP_REPLY 2U
#define IPV4_MIN_HEADER 20U
#define IPV4_SOURCE_OFFSET 12U
#define IPV4_DEST_OFFSET 16U
#define IPV4_PROTOCOL_OFFSET 9U
#define IPV4_FRAGMENT_OFFSET 6U
#define IPV4_FRAGMENT_MASK 0x3FFFU
#define IPV4_RESERVED_FLAG 0x8000U
#define IPV4_OFFSET_MASK 0x1FFFU
#define IP_ICMP 1U
#define IP_TCP 6U
#define IP_UDP 17U
#define TCP_MIN_HEADER 20U
#define TCP_DATA_OFFSET 12U
#define UDP_HEADER_SIZE 8U
#define ICMP_HEADER_SIZE 8U
#define ICMP_ECHO_REPLY 0U
#define ICMP_DEST_UNREACHABLE 3U
#define ICMP_ECHO_REQUEST 8U
#define ICMP_TIME_EXCEEDED 11U
#define ICMP_PARAMETER_PROBLEM 12U

static unsigned read_be16(const uint8_t *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

static int same(const uint8_t *a, const uint8_t *b, unsigned size)
{
    return memcmp(a, b, size) == 0;
}

static int broadcast_ip(const uint8_t *p)
{
    return p[0] == 255U && p[1] == 255U && p[2] == 255U && p[3] == 255U;
}

static int ethernet_destination(const uint8_t *p, const uint8_t *physical,
                                const uint8_t *virtual)
{
    static const uint8_t broadcast[MAC_SIZE] = {255U,255U,255U,255U,255U,255U};
    return same(p, physical, MAC_SIZE) || same(p, virtual, MAC_SIZE) ||
        same(p, broadcast, MAC_SIZE) ||
        (p[0] == 1U && p[1] == 0U && p[2] == 0x5EU && p[3] < 0x80U);
}

/* 引用报文可能仅含原IP头和8字节，不用原totalLength访问未收到的数据。 */
static net_mux_rx_t icmp_owner(const uint8_t *icmp, unsigned length,
                               const uint8_t ip[4], uint16_t pc_port, uint16_t debug_port)
{
    if (length < ICMP_HEADER_SIZE) return NET_MUX_DROP;
    if (icmp[0] == ICMP_ECHO_REQUEST) return NET_MUX_LOCAL;
    if (icmp[0] == ICMP_ECHO_REPLY) return NET_MUX_RK;
    if (icmp[0] != ICMP_DEST_UNREACHABLE && icmp[0] != ICMP_TIME_EXCEEDED &&
        icmp[0] != ICMP_PARAMETER_PROBLEM) return NET_MUX_LOCAL;
    if (length < ICMP_HEADER_SIZE + IPV4_MIN_HEADER) return NET_MUX_DROP;
    const uint8_t *original = icmp + ICMP_HEADER_SIZE;
    unsigned header = (unsigned)(original[0] & 15U) * 4U;
    if ((original[0] >> 4) != 4U || header < IPV4_MIN_HEADER ||
        length < ICMP_HEADER_SIZE + header + UDP_HEADER_SIZE ||
        read_be16(original + 2U) < header + UDP_HEADER_SIZE ||
        (read_be16(original + IPV4_FRAGMENT_OFFSET) & IPV4_OFFSET_MASK) ||
        !same(original + IPV4_SOURCE_OFFSET, ip, 4U)) return NET_MUX_DROP;
    if (original[IPV4_PROTOCOL_OFFSET] == IP_TCP)
        return (read_be16(original + header) == pc_port || read_be16(original + header) == debug_port) ? NET_MUX_LOCAL : NET_MUX_RK;
    return original[IPV4_PROTOCOL_OFFSET] == IP_UDP ? NET_MUX_RK : NET_MUX_LOCAL;
}

static void to_virtual(uint8_t *frame, const uint8_t virtual_mac[6])
{
    /* 广播/组播地址保留，单播包才改成SNP声明的虚拟MAC。 */
    if (!(frame[0] & 1U)) memcpy(frame, virtual_mac, MAC_SIZE);
}

net_mux_rx_t net_mux_rx(uint8_t *frame, uint16_t length,
                       const uint8_t physical_mac[6], const uint8_t virtual_mac[6],
                       const uint8_t ip[4], uint16_t pc_port, uint16_t debug_port)
{
    if (!frame || !physical_mac || !virtual_mac || !ip || !pc_port ||
        length < ETH_HEADER_SIZE || length > SN_ETH_MAX ||
        !ethernet_destination(frame, physical_mac, virtual_mac)) return NET_MUX_DROP;
    unsigned type = read_be16(frame + ETH_TYPE_OFFSET);
    uint8_t *packet = frame + ETH_HEADER_SIZE;
    if (type == ETH_ARP) {
        if (!sn_ip_guard_rx(frame, length, virtual_mac, ip)) return NET_MUX_DROP;
        /* 外部真正声称本机IP的包交给两端检测，不能把其来源伪装成本机。 */
        if (same(packet + ARP_SENDER_IP_OFFSET, ip, 4U)) {
            if (same(packet + ARP_SENDER_MAC_OFFSET, physical_mac, MAC_SIZE) ||
                same(packet + ARP_SENDER_MAC_OFFSET, virtual_mac, MAC_SIZE)) return NET_MUX_LOCAL;
            to_virtual(frame, virtual_mac);
            return NET_MUX_BOTH;
        }
        if (read_be16(packet + ARP_OPERATION_OFFSET) != ARP_REPLY ||
            !same(packet + ARP_TARGET_IP_OFFSET, ip, 4U)) return NET_MUX_LOCAL;
        if (!same(packet + ARP_TARGET_MAC_OFFSET, physical_mac, MAC_SIZE) &&
            !same(packet + ARP_TARGET_MAC_OFFSET, virtual_mac, MAC_SIZE)) return NET_MUX_LOCAL;
        memcpy(packet + ARP_TARGET_MAC_OFFSET, virtual_mac, MAC_SIZE);
        to_virtual(frame, virtual_mac);
        return NET_MUX_BOTH;
    }
    if (type != ETH_IPV4 || length < ETH_HEADER_SIZE + IPV4_MIN_HEADER) return NET_MUX_DROP;
    unsigned header = (unsigned)(packet[0] & 15U) * 4U;
    unsigned total = read_be16(packet + 2U);
    unsigned fragment = read_be16(packet + IPV4_FRAGMENT_OFFSET);
    if ((packet[0] >> 4) != 4U || header < IPV4_MIN_HEADER || total < header ||
        total > length - ETH_HEADER_SIZE || (fragment & IPV4_RESERVED_FLAG)) return NET_MUX_DROP;
    int addressed = same(packet + IPV4_DEST_OFFSET, ip, 4U);
    if (!addressed && !broadcast_ip(packet + IPV4_DEST_OFFSET) &&
        (packet[IPV4_DEST_OFFSET] & 0xF0U) != 0xE0U) return NET_MUX_DROP;
    if (fragment & IPV4_FRAGMENT_MASK)
        return addressed && packet[IPV4_PROTOCOL_OFFSET] == IP_TCP ? NET_MUX_LOCAL : NET_MUX_DROP;
    unsigned payload = total - header;
    uint8_t *transport = packet + header;
    net_mux_rx_t route;
    if (packet[IPV4_PROTOCOL_OFFSET] == IP_TCP) {
        if (!addressed || payload < TCP_MIN_HEADER ||
            (unsigned)(transport[TCP_DATA_OFFSET] >> 4) * 4U < TCP_MIN_HEADER ||
            (unsigned)(transport[TCP_DATA_OFFSET] >> 4) * 4U > payload) return NET_MUX_DROP;
        route = (read_be16(transport + 2U) == pc_port || read_be16(transport + 2U) == debug_port) ? NET_MUX_LOCAL : NET_MUX_RK;
    } else if (packet[IPV4_PROTOCOL_OFFSET] == IP_UDP) {
        if (!sn_ip_guard_rx(frame, length, virtual_mac, ip)) return NET_MUX_DROP;
        route = NET_MUX_RK;
    } else if (packet[IPV4_PROTOCOL_OFFSET] == IP_ICMP) {
        if (!addressed) return NET_MUX_DROP;
        route = icmp_owner(transport, payload, ip, pc_port, debug_port);
    } else return NET_MUX_LOCAL;
    if (route == NET_MUX_RK) to_virtual(frame, virtual_mac);
    return route;
}

net_mux_tx_t net_mux_tx(uint8_t *frame, uint16_t length,
                       const uint8_t physical_mac[6], const uint8_t virtual_mac[6],
                       const uint8_t ip[4], uint16_t pc_port, uint16_t debug_port)
{
    if (!physical_mac || !virtual_mac || !ip || !pc_port ||
        !sn_ip_guard_tx(frame, length, virtual_mac, ip)) return NET_MUX_INVALID;
    uint8_t *packet = frame + ETH_HEADER_SIZE;
    if (read_be16(frame + ETH_TYPE_OFFSET) == ETH_ARP) {
        if (read_be16(packet + ARP_OPERATION_OFFSET) == ARP_REPLY ||
            same(packet + ARP_TARGET_IP_OFFSET, ip, 4U)) return NET_MUX_SUPPRESS;
        memcpy(packet + ARP_SENDER_MAC_OFFSET, physical_mac, MAC_SIZE);
    } else {
        unsigned header = (unsigned)(packet[0] & 15U) * 4U;
        unsigned payload = read_be16(packet + 2U) - header;
        const uint8_t *transport = packet + header;
        if (packet[IPV4_PROTOCOL_OFFSET] == IP_TCP) {
            if (payload < TCP_MIN_HEADER ||
                (unsigned)(transport[TCP_DATA_OFFSET] >> 4) * 4U < TCP_MIN_HEADER ||
                (unsigned)(transport[TCP_DATA_OFFSET] >> 4) * 4U > payload ||
                (read_be16(transport) == pc_port || read_be16(transport) == debug_port)) return NET_MUX_INVALID;
        } else if (packet[IPV4_PROTOCOL_OFFSET] == IP_UDP) {
            if (payload < UDP_HEADER_SIZE || read_be16(transport + 4U) != payload)
                return NET_MUX_INVALID;
        } else if (packet[IPV4_PROTOCOL_OFFSET] == IP_ICMP) {
            if (payload < ICMP_HEADER_SIZE) return NET_MUX_INVALID;
            if (transport[0] == ICMP_ECHO_REPLY) return NET_MUX_SUPPRESS;
        } else return NET_MUX_INVALID;
    }
    memcpy(frame + MAC_SIZE, physical_mac, MAC_SIZE);
    return NET_MUX_SEND;
}
