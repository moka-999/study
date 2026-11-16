#ifndef SERIAL_NET_MUX_H
#define SERIAL_NET_MUX_H

#include "serial_net.h"

typedef enum {
    NET_MUX_DROP = 0,
    NET_MUX_LOCAL,
    NET_MUX_RK,
    NET_MUX_BOTH
} net_mux_rx_t;

typedef enum {
    NET_MUX_INVALID = 0,
    NET_MUX_SEND,
    NET_MUX_SUPPRESS
} net_mux_tx_t;

/* 输入无FCS/无VLAN的以太帧副本，length为实际字节数，ip为网络序4字节。
 * MCU独占pc_port/debug_port对应TCP服务和ARP应答；RK使用其余TCP及UDP。
 * RK/BOTH只改副本的目的MAC和ARP目标MAC；BOTH的原包仍须交MCU。
 * TCP分片全部交MCU重组，RK暂不支持分片；不替代协议栈的校验和检查。
 */
net_mux_rx_t net_mux_rx(uint8_t *frame, uint16_t length,
                       const uint8_t physical_mac[6], const uint8_t virtual_mac[6],
                       const uint8_t ip[4], uint16_t pc_port, uint16_t debug_port);

/* SEND已把源MAC/ARP发送者MAC改为MCU；SUPPRESS表示无需上网且应答成功。
 * DHCP chaddr及IP/TCP/UDP字节保持原样，因此不重算其校验和。
 * RK禁止占用pc_port/debug_port作为TCP源端口，且不回答ARP或再次宣告本机地址。
 */
net_mux_tx_t net_mux_tx(uint8_t *frame, uint16_t length,
                       const uint8_t physical_mac[6], const uint8_t virtual_mac[6],
                       const uint8_t ip[4], uint16_t pc_port, uint16_t debug_port);

#endif
