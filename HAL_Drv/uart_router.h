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

/**
 * @brief 调度前为逻辑UART槽注册接口并创建接收队列。
 * @param[in] id 槽编号0或1。
 * @param[in] interface 借用接口，须含start_rx/send并覆盖槽位使用期。
 * @note 无效参数、重复注册或分配失败直接返回；无状态码，失败槽不可用。
 *       每槽只注册一次；注册不启动物理接收。
 */
void uart_register(int id, uart_if_t *interface);

/**
 * @brief 调度前创建TX互斥锁、挂接HAL TxISR并启动已注册槽接收。
 * @note 调用方须先初始化外设。锁分配失败时send拒绝发送；当前启动RX的
 *       返回值未向上传递，因此本函数返回不等于所有DMA都已成功启动。
 */
void uart_start_all(void);

/**
 * @brief 在任务上下文无等待取走一个UART接收块。
 * @param[in] id 已注册槽编号0或1。
 * @param[out] buffer 非NULL调用方缓冲，至少UART_RX_BUF_SIZE字节。
 * @param[in] capacity 缓冲实际字节容量；小于要求时不出队。
 * @return 实际复制字节数；无数据或参数无效返回0，输出保持原值。
 * @note 输出是复制的数据，可立即交状态机；一个接收块不保证是一帧。
 */
int uart_poll_rx(int id, uint8_t *buffer, uint16_t capacity);

/**
 * @brief 在任务上下文将载荷交给已注册UART发送接口。
 * @param[in] id 已注册槽编号0或1。
 * @param[in] buffer 非NULL，至少可读length字节，当前STM32实现复制入TX环。
 * @param[in] length 非0字节数，当前TX环最大可接纳2047字节。
 * @return IF_OK已复制入TX环；IF_ERR参数/槽无效、锁失败或环空间不足。
 * @note 当前STM32实现取锁最多100ms；失败不入队部分载荷。IF_OK不代表线上
 *       发完或RK收到；调用方须决定失败重试/丢弃策略，禁止ISR调用。
 */
int uart_send(int id, uint8_t *buffer, uint16_t length);

#endif /* UART_ROUTER_H */
