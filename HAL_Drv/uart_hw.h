/* STM32 UART适配层内部入口；普通任务应使用uart_router.h。 */
#ifndef UART_HW_H
#define UART_HW_H
#include "hal_if.h"

/*
 * 在启动接收前创建TX互斥锁。重复调用不重复分配；若分配失败，
 * 该串口send返回IF_ERR，不能降级为不受保护的多任务写入。
 */
void uart_tx_mutex_init(void);

/* 在启动RX前绑定STM32 HAL TxISR，避免与接收DMA状态锁冲突。 */
void uart_tx_isr_attach(void);

/* 仅任务上下文停止/清理RX，保留TX环及TX中断；IF_OK成功，否则仍需重试。 */
int uart_rx_abort(uart_if_t *port);

#endif /* UART_HW_H */
