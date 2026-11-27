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

/* 已绑定循环DMA的端口专用。读取下一写入位置0..size-1，满圈边界返回0。 */
uint16_t uart_hw_rx_position(uart_if_t *port, uint16_t size);

/* 路由层专用：单端口最多一帧待完成；ticket非0，完成时间来自硬件TC中断。 */
int uart_hw_send_tracked(uart_if_t *port, uint8_t *buffer, uint16_t length,
                          uint32_t *ticket);
/* 1已发完/0仍在途/-1无效票据；只有1时写completed_ms。任务上下文。 */
int uart_hw_tx_status(uart_if_t *port, uint32_t ticket, uint32_t *completed_ms);

/* 1表示TX环与硬件移位寄存器均空；不改变TC标记。 */
int uart_hw_tx_idle(uart_if_t *port);
/* 改速前被拒时RX仍在运行；只有确认停止后才能从零重置DMA写位置。 */
typedef enum {
    UART_BAUD_CHANGED,    /* 已切到新速率，RX已停。 */
    UART_BAUD_UNCHANGED,  /* 参数、互斥锁或TX空闲检查未通过，未操作RX。 */
    UART_BAUD_RX_STOPPED, /* RX已停，新速率初始化失败，已尝试恢复原速率。 */
    UART_BAUD_RX_FAULT    /* RX停止失败，交给任务恢复。 */
} uart_baud_result_t;
uart_baud_result_t uart_hw_set_baud(uart_if_t *port, uint32_t baud);

#endif /* UART_HW_H */
