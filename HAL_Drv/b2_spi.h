#ifndef B2_SPI_H
#define B2_SPI_H

#include "b2_protocol.h"

typedef struct {
    uint32_t token;               /* 本地CS代次，不上协议线；防止旧任务响应覆盖新请求。 */
    uint16_t length;
    uint8_t data[B2_MAX_FRAME];
} b2_spi_packet_t;

typedef struct {
    uint32_t transactions;
    uint32_t hardware_errors;
    uint32_t queue_drops;
    uint32_t received_requests;
    uint32_t fifo_events;         /* 出现FEIF的CS事务数，包含主机全FF读轮询，不等于丢帧数。 */
    uint32_t stop_timeouts;       /* 停DMA的有界等待失败次数；此时禁止重装缓冲。 */
    uint32_t last_spi_status;     /* 最近一次CS结束时的SPI_SR原始值，清外设前保存。 */
    uint32_t last_rx_dma_status;  /* 同笔事务的DMA_LISR，含接收Stream3标志。 */
    uint32_t last_tx_dma_status;  /* 同笔事务的DMA_HISR，含发送Stream4标志。 */
    uint32_t last_length;         /* 最近事务补收RX FIFO后的字节数；停DMA失败时为0。 */
} b2_spi_diagnostics_t;

/** 调度后由唯一B2任务初始化。B1用SPI2，V2用SPI3；1成功，0失败且不启用中断。 */
int b2_spi_start(void);
/** 非阻塞取一份完整CS事务。1取到，0无；不会借用正在DMA接收的缓冲。 */
int b2_spi_receive(b2_spi_packet_t *packet);
/** 发布应答候选，最多37字节。只在后续CS上升沿换入TX，绝不修改CS低期间的数据。
 * 首次读可能仍全FF，主机按5ms间隔继续读；请求token已过时则返回0。
 */
int b2_spi_publish(uint32_t token, const uint8_t *frame, size_t length);
/** 只读复制ISR计数，不清零；仅任务上下文使用。 */
void b2_spi_diagnostics(b2_spi_diagnostics_t *out);

#endif
