#include "ip_guard.h"

#define ETH_HEADER_SIZE 14U
#define ETH_IPV4 0x0800U
#define ETH_ARP 0x0806U
#define IPV4_MIN_HEADER 20U
#define IPV4_UDP 17U
#define IPV4_FRAGMENT_MASK 0xBFFFU
#define UDP_HEADER_SIZE 8U
#define DHCP_SERVER_PORT 67U
#define DHCP_CLIENT_PORT 68U
#define BOOTP_FIXED_SIZE 236U
#define DHCP_FIXED_SIZE 240U
#define BOOTP_CLIENT_MAC_OFFSET 28U
#define BOOTP_YOUR_IP_OFFSET 16U
#define ARP_SIZE 28U

static unsigned read_be16(const uint8_t *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

static int equal(const uint8_t *a, const uint8_t *b, unsigned length)
{
    for (unsigned i = 0U; i < length; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

static int zero_ip(const uint8_t *p)
{
    return (p[0] | p[1] | p[2] | p[3]) == 0U;
}

/* DHCP包必须包含完整BOOTP头及cookie，避免把任意UDP源0当成初始化许可。 */
static int bootp_valid(const uint8_t *data, unsigned length,
                       const uint8_t mac[6], unsigned operation)
{
    static const uint8_t cookie[] = {99U, 130U, 83U, 99U};
    return length >= DHCP_FIXED_SIZE && data[0] == operation && data[1] == 1U &&
           data[2] == 6U && equal(data + BOOTP_CLIENT_MAC_OFFSET, mac, 6U) &&
           equal(data + BOOTP_FIXED_SIZE, cookie, sizeof(cookie));
}

/* 返回协议类型；所有长度先校验，后续不依赖DMA数据中的未校验偏移。 */
static unsigned inspect(const uint8_t *frame, uint16_t length, unsigned *header,
                         unsigned *total)
{
    if (!frame || length < ETH_HEADER_SIZE || length > SN_ETH_MAX) return 0U;
    unsigned type = read_be16(frame + 12U);
    if (type == ETH_ARP) {
        const uint8_t *arp = frame + ETH_HEADER_SIZE;
        if (length < ETH_HEADER_SIZE + ARP_SIZE || read_be16(arp) != 1U ||
            read_be16(arp + 2U) != ETH_IPV4 || arp[4] != 6U || arp[5] != 4U ||
            (read_be16(arp + 6U) != 1U && read_be16(arp + 6U) != 2U)) return 0U;
        return type;
    }
    if (type != ETH_IPV4 || length < ETH_HEADER_SIZE + IPV4_MIN_HEADER) return 0U;
    const uint8_t *packet = frame + ETH_HEADER_SIZE;
    *header = (unsigned)(packet[0] & 15U) * 4U;
    *total = read_be16(packet + 2U);
    if ((packet[0] >> 4) != 4U || *header < IPV4_MIN_HEADER || *total < *header ||
        *total > length - ETH_HEADER_SIZE ||
        (read_be16(packet + 6U) & IPV4_FRAGMENT_MASK)) return 0U;
    return type;
}

int sn_ip_guard_tx(const uint8_t *frame, uint16_t length,
                   const uint8_t mac[6], const uint8_t ip[4])
{
    unsigned header = 0U, total = 0U;
    unsigned type = inspect(frame, length, &header, &total);
    if (!type || !mac || !ip || !equal(frame + 6U, mac, 6U)) return 0;
    const uint8_t *packet = frame + ETH_HEADER_SIZE;
    if (type == ETH_ARP) {
        if (!equal(packet + 8U, mac, 6U)) return 0;
        if (equal(packet + 14U, ip, 4U)) return 1;
        /* 地址探测只能针对本轮授权IP，不能用源0探测后宣告其它地址。 */
        return zero_ip(packet + 14U) && read_be16(packet + 6U) == 1U &&
               equal(packet + 24U, ip, 4U);
    }
    if (equal(packet + 12U, ip, 4U)) return 1;
    if (!zero_ip(packet + 12U) || packet[9] != IPV4_UDP ||
        total < header + UDP_HEADER_SIZE) return 0;
    const uint8_t *udp = packet + header;
    unsigned size = read_be16(udp + 4U);
    return size == total - header && read_be16(udp) == DHCP_CLIENT_PORT &&
           read_be16(udp + 2U) == DHCP_SERVER_PORT &&
           bootp_valid(udp + UDP_HEADER_SIZE, size - UDP_HEADER_SIZE, mac, 1U);
}

int sn_ip_guard_rx(const uint8_t *frame, uint16_t length,
                   const uint8_t mac[6], const uint8_t ip[4])
{
    unsigned header = 0U, total = 0U;
    unsigned type = inspect(frame, length, &header, &total);
    if (!type || !mac || !ip) return 0;
    if (type == ETH_ARP) return 1;
    const uint8_t *packet = frame + ETH_HEADER_SIZE;
    if (packet[9] != IPV4_UDP) return 1;
    if (total < header + UDP_HEADER_SIZE) return 0;
    const uint8_t *udp = packet + header;
    unsigned size = read_be16(udp + 4U);
    if (size != total - header) return 0;
    if (read_be16(udp + 2U) != DHCP_CLIENT_PORT) return 1;
    if (read_be16(udp) != DHCP_SERVER_PORT ||
        !bootp_valid(udp + UDP_HEADER_SIZE, size - UDP_HEADER_SIZE, mac, 2U)) return 0;
    const uint8_t *offered = udp + UDP_HEADER_SIZE + BOOTP_YOUR_IP_OFFSET;
    return zero_ip(offered) || equal(offered, ip, 4U);
}
