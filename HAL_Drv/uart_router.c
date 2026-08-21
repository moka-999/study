/* 多串口路由器 — 面向 uart_if_t 接口 */
#include "main.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "hal_if.h"
#include "uart_router.h"
#include <string.h>

#define UART_NUM 2

/* 队列消息: 长度前缀 + 数据 (data 须 >= rx, 否则满载时溢出) */
typedef struct { uint16_t len; uint8_t data[UART_RX_BUF_SIZE]; } uart_msg_t;

typedef struct {
  uart_if_t   *iface;
  QueueHandle_t q;
  uint8_t       rx[UART_RX_BUF_SIZE];
} uart_slot_t;

/* slots 整体放 .uart_dma 段 (RAM_D2): rx 是 DMA 目标缓冲, DTCM 够不着 */
static uart_slot_t slots[UART_NUM] __attribute__((section(".uart_dma")));
static int         count = 0;

/* 统一空闲中断回调 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *h, uint16_t sz)
{
  int i, id = -1;
  for(i=0;i<count;i++) if(slots[i].iface->handle == h) { id=i; break; }
  if(id<0||sz==0||sz>UART_RX_BUF_SIZE) return;
  /* TLV 协议: 不过滤单字节, 交给上层状态机处理同步/噪声 */
  uart_msg_t m;
  m.len = sz; memcpy(m.data, slots[id].rx, sz);
  BaseType_t xh = pdFALSE;
  xQueueSendFromISR(slots[id].q, &m, &xh);
  portYIELD_FROM_ISR(xh);
  slots[id].iface->start_rx(slots[id].iface, slots[id].rx, sizeof(slots[id].rx));
}

void uart_register(int idx, uart_if_t *iface)
{
  if(idx>=UART_NUM) return;
  slots[idx].iface = iface;
  slots[idx].q     = xQueueCreate(8, sizeof(uart_msg_t));
  if(idx >= count) count = idx+1;
}

void uart_start_all(void)
{
  int i;
  uart_tx_isr_attach();   /* 绑定 TxISR, 让 HAL 的 TXE 中断自动排空发送环形缓冲 */
  for(i=0;i<count;i++) slots[i].iface->start_rx(slots[i].iface, slots[i].rx, UART_RX_BUF_SIZE);
}

int uart_poll_rx(int id, uint8_t *buf, uint16_t m)
{
  if(id>=count||m<UART_RX_BUF_SIZE) return 0;
  uart_msg_t msg;
  if(xQueueReceive(slots[id].q, &msg, 0) != pdTRUE) return 0;
  memcpy(buf, msg.data, msg.len);
  return (int)msg.len;
}

int uart_send(int id, uint8_t *d, uint16_t l)
{ if(id<count) return slots[id].iface->send(slots[id].iface, d, l); return -1; }
