/*
 * ===================================================================
 * 多串口路由器 — UART↔TCP 网桥
 * ===================================================================
 *
 * 功能:
 *   所有已注册 UART 的接收数据通过空闲中断入队,
 *   TCP 任务统一轮询各队列, 转发到 PC。
 *   PC 下发的数据通过 uart_send() 写到指定串口。
 *
 * 使用流程:
 *   1. uart_register(0, &uart1_if)  — 注册 USART1
 *   2. uart_register(1, &uart2_if)  — 注册 USART2
 *   3. uart_start_all()             — 启动空闲中断接收
 *   4. uart_poll_rx(0, buf, len)    — TCP 任务轮询数据
 *   5. uart_send(0, buf, len)       — PC 下发 → 串口
 *
 * 换 MCU 移植: 只需确保 uart_if_t 实例已注册, 本模块不改
 * ===================================================================
 */
#ifndef __UART_ROUTER_H
#define __UART_ROUTER_H

#include "hal_if.h"

/* DMA RX 缓冲大小 = TLV 单帧上限(1040) 向上对齐 32。
 * DMA 缓冲必须能装下完整一帧, 否则收满触发回调会把一帧拆成两段喂给状态机
 * (之前 256B 放不下 1040B 满帧的隐患)。DMA 缓冲本身放 .uart_dma 段(RAM_D2)。 */
#define UART_RX_BUF_SIZE  1056

void uart_register(int id, uart_if_t *iface);
void uart_start_all(void);
int  uart_poll_rx(int id, uint8_t *buf, uint16_t maxlen);
int  uart_send(int id, uint8_t *data, uint16_t len);  /* 0=ok, -1=buffer full */

#endif
