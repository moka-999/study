/*
 * ===================================================================
 * UART 接口 — STM32 HAL 实现
 * ===================================================================
 * uart1: PA9=TX, PA10=RX (阿波罗:CH340→PC / B1:RS0102→RK3588)
 * uart2: PD5=TX, PD6=RX  (预留)
 *
 * 发送: 中断驱动, 环形缓冲区, 非阻塞
 *   任务层 _send() → ring buffer → TXE ISR drain
 *   不持有 HAL Lock, 不与 RX 的 HAL_UARTEx_ReceiveToIdle_DMA 冲突
 *
 * 接收: IDLE 空闲中断 → uart_router ISR 回调 → 队列 → task
 *
 * ★ 多任务写入安全: FreeRTOS mutex 保护 ring buffer
 * ===================================================================
 */
#include "hal_if.h"
#include "usart.h"
#include "FreeRTOS.h"
#include "semphr.h"

/* ── TX 环形缓冲区 (多生产者单消费者, 需 mutex) ── */
#define TX_RING_SIZE  2048  /* 必须是2的幂, ≥ 1032 (TLV_FRAME_MAX 实际上限) */

typedef struct {
    uint8_t           buf[TX_RING_SIZE];
    volatile uint16_t head;   /* ISR 读指针    */
    volatile uint16_t tail;   /* 任务层写指针   */
    SemaphoreHandle_t mutex;  /* 多任务写保护   */
} uart_tx_ring_t;

static uart_tx_ring_t _tx1, _tx2;

/* 初始化 TX ring buffer 的 mutex (在 FreeRTOS 调度器启动前调用) */
void uart_tx_mutex_init(void)
{
    _tx1.mutex = xSemaphoreCreateMutex();
    _tx2.mutex = xSemaphoreCreateMutex();
}

/* 根据 HAL 句柄查环形缓冲区 */
static inline uart_tx_ring_t *_ring(UART_HandleTypeDef *h)
{
    if (h == &huart1) return &_tx1;
    if (h == &huart2) return &_tx2;
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * 中断 TX drain (ISR 上下文, HAL_UART_IRQHandler 之后调用)
 * ═══════════════════════════════════════════════════════════════════ */

void uart_tx_drain(UART_HandleTypeDef *huart)
{
    uart_tx_ring_t *r = _ring(huart);
    if (!r) return;

    USART_TypeDef *u = huart->Instance;

    /* TXE=1 且缓冲区有数据 → 填充 DR */
    while (u->ISR & USART_ISR_TXE_TXFNF) {
        if (r->head == r->tail) {
            /* 缓冲区空: 关闭 TXE 中断, 等下次 send 再开 */
            u->CR1 &= ~USART_CR1_TXEIE;
            break;
        }
        u->TDR = r->buf[r->head];
        r->head = (uint16_t)(r->head + 1) & (TX_RING_SIZE - 1);
    }
}

/* 把 drain 挂到 HAL 的 TxISR 钩子:
 * HAL_UART_IRQHandler 的 TXE 分支会调 huart->TxISR, 这样 USART ISR 无需任何自定义代码,
 * 也彻底摆脱 "CubeMX 重新生成清空 stm32h7xx_it.c USER CODE 区导致 drain 丢失" 的坑 */
void uart_tx_isr_attach(void)
{
    huart1.TxISR = uart_tx_drain;
    huart2.TxISR = uart_tx_drain;
}

/* ═══════════════════════════════════════════════════════════════════
 * 非阻塞发送 (任务上下文)
 * ═══════════════════════════════════════════════════════════════════ */

static int _send(uart_if_t *p, uint8_t *d, uint16_t l)
{
    uart_tx_ring_t *r = _ring((UART_HandleTypeDef *)p->handle);
    if (!r || l == 0 || l >= TX_RING_SIZE) return -1;

    /* ★ 取 mutex (如果已创建); 单生产者时跳过 ★ */
    int need_unlock = 0;
    if (r->mutex) {
        if (xSemaphoreTake(r->mutex, pdMS_TO_TICKS(100)) != pdTRUE)
            return -1;
        need_unlock = 1;
    }

    /* 整帧空间检查 */
    uint16_t space = (uint16_t)(r->head - r->tail - 1) & (TX_RING_SIZE - 1);
    if (l > space) {
        if (need_unlock) xSemaphoreGive(r->mutex);
        return -1;
    }

    int was_empty = (r->head == r->tail);

    for (uint16_t i = 0; i < l; i++) {
        r->buf[r->tail] = d[i];
        r->tail = (uint16_t)(r->tail + 1) & (TX_RING_SIZE - 1);
    }

    if (was_empty) {
        UART_HandleTypeDef *h = (UART_HandleTypeDef *)p->handle;
        __disable_irq();
        __HAL_UART_ENABLE_IT(h, UART_IT_TXE);   /* 触发 TxISR(=uart_tx_drain) 排空 */
        __enable_irq();
    }

    if (need_unlock) xSemaphoreGive(r->mutex);
    return 0;
}

/* ── 接收 (DMA + IDLE; 缓冲须在 RAM_D2 供 DMA 访问) ── */
static int _rx(uart_if_t *p, uint8_t *buf, uint16_t sz)
{ return HAL_UARTEx_ReceiveToIdle_DMA(p->handle, buf, sz) == HAL_OK ? IF_OK : IF_ERR; }

/* ── 工厂实例 ── */
uart_if_t uart1_if = { .handle=&huart1, .send=_send, .start_rx=_rx };
uart_if_t uart2_if = { .handle=&huart2, .send=_send, .start_rx=_rx };
