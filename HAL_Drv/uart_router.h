/*
 * UART字节路由接口：不承载TCP转发，不解释RK/PC协议。
 * V2逻辑槽0绑定USART1作RK对测，B1的物理UART选择另行确认。
 */
#ifndef UART_ROUTER_H
#define UART_ROUTER_H

#include "hal_if.h"

/*
 * 接收块容量1056字节，覆盖当前RK最大帧1040字节；状态机仍必须支持分片。
 * DMA RX缓冲位于.uart_dma段，不能放DTCM。容量不代表一次回调必为完整帧。
 */
#define UART_RX_BUF_SIZE 1056

/* 调度前每槽注册一次。id为0或1；非法参数/分配失败不会创建可用槽位。 */
void uart_register(int id, uart_if_t *interface);

/* 调度前初始化TX锁/中断钩子，再启动已注册槽位接收；调用方负责外设已就绪。 */
void uart_start_all(void);

/*
 * 任务上下文非等待取一个接收块；buffer至少有UART_RX_BUF_SIZE字节。
 * 返回实际复制字节数，0表示无数据或参数无效；不会返回内部DMA指针。
 */
int uart_poll_rx(int id, uint8_t *buffer, uint16_t capacity);

/*
 * 任务上下文发送；IF_OK表示已复制入TX环，不代表物理发送完毕。
 * 锁竞争最多等待100ms；无效槽/参数、锁失败或TX环满返回IF_ERR。
 * 失败时调用方必须显式决定重试或丢弃，禁止在ISR调用。
 */
int uart_send(int id, uint8_t *buffer, uint16_t length);

#endif /* UART_ROUTER_H */
