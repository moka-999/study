#include "network_config.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <string.h>

extern struct netif gnetif;
extern ip4_addr_t ipaddr, netmask, gw;
extern uint8_t IP_ADDRESS[4], NETMASK_ADDRESS[4], GATEWAY_ADDRESS[4];

/* 单一TCP调用者、静态消息生命周期。已经入队后等待完成，不以超时伪报“未执行”。 */
static struct {
    ip4_addr_t ip, mask, gateway;
    SemaphoreHandle_t completed;
} s_change;

static void apply_in_tcpip(void *argument)
{
    (void)argument;
    netif_set_addr(&gnetif, &s_change.ip, &s_change.mask, &s_change.gateway);
    ipaddr = s_change.ip; netmask = s_change.mask; gw = s_change.gateway;
    IP_ADDRESS[0] = 192U; IP_ADDRESS[1] = 168U; IP_ADDRESS[2] = 210U;
    IP_ADDRESS[3] = ip4_addr4(&s_change.ip);
    NETMASK_ADDRESS[0] = 255U; NETMASK_ADDRESS[1] = 255U;
    NETMASK_ADDRESS[2] = 255U; NETMASK_ADDRESS[3] = 0U;
    memset(GATEWAY_ADDRESS, 0, 4U);
    /* netif_set_addr负责ARP/IP状态通知；保持INADDR_ANY的8888监听socket。 */
    (void)xSemaphoreGive(s_change.completed);
}

int network_config_set_tester(uint8_t tester, uint8_t site)
{
    if (tester < 1U || tester > 21U || site < 1U || site > 8U ||
        netif_ip4_addr(&gnetif)->addr == 0U) return 0;
    if (!s_change.completed) s_change.completed = xSemaphoreCreateBinary();
    if (!s_change.completed) return 0;
    IP4_ADDR(&s_change.ip, 192, 168, 210, 10U * tester + site);
    IP4_ADDR(&s_change.mask, 255, 255, 255, 0);
    IP4_ADDR(&s_change.gateway, 0, 0, 0, 0);
    if (tcpip_try_callback(apply_in_tcpip, NULL) != ERR_OK) return 0;
    /* 协议100ms是主机观察期，不是强行取消已提交配置的期限；超时重试须幂等。 */
    return xSemaphoreTake(s_change.completed, portMAX_DELAY) == pdPASS;
}
