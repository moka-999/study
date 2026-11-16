#ifndef SERIAL_NET_IP_GUARD_H
#define SERIAL_NET_IP_GUARD_H
#include "serial_net.h"

/* 对完整无FCS/无VLAN以太帧校验：1允许，0拒绝；不改数据、不代替IP/UDP校验和。
 * mac为本轮RK六字节地址，ip为MCU授权IPv4的四个网络序字节。
 * 首版仅IPv4/ARP，拒绝IPv4分片；DHCP初始源0合法，普通流量只允许源授权IP。
 */
int sn_ip_guard_tx(const uint8_t *frame, uint16_t length,
                   const uint8_t mac[6], const uint8_t ip[4]);
/* 普通接收不限制PC源IP；DHCP非零yiaddr必须等于授权IP，保留零地址Proxy Offer。 */
int sn_ip_guard_rx(const uint8_t *frame, uint16_t length,
                   const uint8_t mac[6], const uint8_t ip[4]);
#endif
