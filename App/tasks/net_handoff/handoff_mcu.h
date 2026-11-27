#ifndef HANDOFF_MCU_H
#define HANDOFF_MCU_H

#include <stdint.h>

struct netif;
struct pbuf;

typedef enum { HANDOFF_COMMAND_NONE = 0, HANDOFF_COMMAND_RK, HANDOFF_COMMAND_MCU } handoff_command_t;
/* 启动期分配队列；不创建第二个UART任务。 */
int handoff_mcu_prepare(void);
void handoff_mcu_attach(struct netif *netif);
/* 控制台只登记命令；唯一TCP上下文负责检查门禁及网络会话切换。 */
int handoff_mcu_console_request(handoff_command_t command);
void handoff_mcu_tcp_poll(int connected, int listener_open);
int handoff_mcu_listen_allowed(void);
int handoff_mcu_busy(void);
/* 返回1表示协议口仍由SPL使用；预约本身不占UART，发出跳转或开始恢复后返回0。 */
int handoff_mcu_tlv_allowed(void);
/* 0104停止转发并撤销预约；真实OFF后沿用网络排空/115200恢复，不等待EDK RELEASE。 */
void handoff_mcu_stop(void);
/* 正式SLT只在共存模式使用；cookie为本轮非零标识。begin预留但不替换电源流程，
 * prepared在普通时序全部发送后允许接管UART；返回1成功、0冲突/不支持。 */
typedef enum {
    HANDOFF_SLT_WAITING = 0, HANDOFF_SLT_EARLY_EDK,
    HANDOFF_SLT_RELEASED, HANDOFF_SLT_OFF_READY, HANDOFF_SLT_FAILED
} handoff_slt_status_t;
int handoff_slt_begin(uint32_t cookie);
int handoff_slt_prepared(uint32_t cookie);
/* RELEASED证明本会话最终归还应答已TC；OFF_READY另要求真实OFF及115200恢复。
 * EARLY_EDK表示参数准备前已收到EDK HELLO，不代表普通时序已经生效。
 * FAILED含跳转/改速错误及prepared后60秒未完成HELLO授权，不设置测试执行时限。 */
handoff_slt_status_t handoff_slt_status(uint32_t cookie);
/* 仅撤销尚未跳转/识别EDK的预留，返回1；可能已运行EDK则返回0并保留占用。 */
int handoff_slt_cancel(uint32_t cookie);
/* ARMED探测SPL时允许既有0501/Boot/电压配置；发出跳转或识别EDK后只允许下电。 */
int handoff_mcu_power_allowed(int off_command);
/* 返回1表示已消费本轮UART；调用方须跳过TLV/心跳但继续喂任务健康标志。 */
int handoff_mcu_rk_poll(void);
/* 唯一RK任务在正常TLV接收中旁路识别自动前级HELLO；返回1须停本轮TLV发送。
 * 未识别HELLO时不取走字节，Freq及普通时序仍由原RK任务处理。 */
int handoff_mcu_rk_observe(uint8_t byte);
/* 正常TLV消费完成或UART断流后调用，只清旁路COBS半包。 */
void handoff_mcu_rk_reset_probe(void);
/* 同一RK任务在poll返回1后调用；数据期可被RX唤醒，空闲/控制期最多等待1ms。 */
void handoff_mcu_rk_wait(void);
/* 返回1由调用方释放pbuf；共存时TCP监听端口/ARP仍交lwIP，RK单播仅进串口队列。
 * 旧RK MAC命中的本地帧会就地修复目的MAC，保留已有PC连接。 */
int handoff_mcu_input(struct pbuf *p);
/* 仅tcpip线程检查以太网TX描述符；1表示DMA已不持有任何待发帧。 */
int handoff_eth_tx_idle(void);
#endif
