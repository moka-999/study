#ifndef NETWORK_CONFIG_H
#define NETWORK_CONFIG_H

#include <stdint.h>

/** 只允许TCP任务在无客户端、无测试/升级时调用；1已完成，0未执行。
 * 产线网段取自 B1_PRODUCTION_SUBNET，/24，末段为 10*tester+site，网关 0.0.0.0。
 * tester 范围 1..21，site 范围 1..8。
 * 不重建lwIP、不更换MAC、不写Flash。通过tcpip线程执行netif_set_addr。
 */
int network_config_set_tester(uint8_t tester, uint8_t site);

#endif
