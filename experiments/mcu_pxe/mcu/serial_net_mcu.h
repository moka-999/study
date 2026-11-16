#ifndef SERIAL_NET_MCU_H
#define SERIAL_NET_MCU_H
#include "lwip/netif.h"
#include "lwip/pbuf.h"
/* 调度前建队列；失败返回0，由启动流程停机。 */
int serial_net_prepare(void);
/* MAC初始化后注册虚拟MAC目的地址过滤，不能开启混杂模式。 */
void serial_net_attach(struct netif *netif);
/* 以太网收包线程调用：1=仅给RK的包，调用方释放；0=继续交MCU的lwIP。 */
int serial_net_input(struct pbuf *packet);
void StartSerialNetTask(void *argument);
#endif
