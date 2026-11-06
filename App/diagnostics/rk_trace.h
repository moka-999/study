#ifndef B1_RK_TRACE_H
#define B1_RK_TRACE_H

#include "rk_link.h"

/* RX借用已解包视图，TX借用实际交给UART的整帧。queued只表示本地发送入队。 */
void rk_trace_received(const tlv_frame_t *frame);
void rk_trace_query_while_busy(const tlv_frame_t *frame);
void rk_trace_sent(uint16_t command, const uint8_t *wire, int length, int queued);
void rk_trace_rejected(rk_rx_reject_reason_t reason, const tlv_frame_t *frame);
void rk_trace_progress(const tlv_frame_t *frame, int previous_valid, unsigned percent);
/* queried为0/1表示本轮未查询/已尝试应答，-1表示未绑定启动频率。 */
void rk_trace_ready(uint64_t sequence, int queried);

#endif
