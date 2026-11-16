#ifndef HANDOFF_MCU_H
#define HANDOFF_MCU_H

struct netif;
struct pbuf;

typedef enum { HANDOFF_COMMAND_NONE = 0, HANDOFF_COMMAND_RK, HANDOFF_COMMAND_MCU } handoff_command_t;
/* 启动期分配队列；不创建第二个UART任务。 */
int handoff_mcu_prepare(void);
void handoff_mcu_attach(struct netif *netif);
/* 控制台只登记命令；唯一TCP上下文负责检查门禁与地址切换。 */
int handoff_mcu_console_request(handoff_command_t command);
void handoff_mcu_tcp_poll(int connected, int listener_open);
int handoff_mcu_listen_allowed(void);
int handoff_mcu_busy(void);
/* ARMED探测SPL时允许既有0501/Boot/电压配置；发出跳转或识别EDK后只允许下电。 */
int handoff_mcu_power_allowed(int off_command);
/* 返回1表示已消费本轮UART；调用方须跳过TLV/心跳但继续喂任务健康标志。 */
int handoff_mcu_rk_poll(void);
/* 返回1由调用方释放pbuf；广播只复制，不阻断MCU正常lwIP输入。 */
int handoff_mcu_input(struct pbuf *p);
/* 仅tcpip线程检查以太网TX描述符；1表示DMA已不持有任何待发帧。 */
int handoff_eth_tx_idle(void);
#endif
