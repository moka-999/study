#ifndef B1_RK_TRACE_H
#define B1_RK_TRACE_H

#include "rk_link.h"
#include "idd_session.h"

/* RX借用已解包视图，TX借用实际交给UART的整帧。queued只表示本地发送入队。 */
void rk_trace_received(const tlv_frame_t *frame);
void rk_trace_query_while_busy(const tlv_frame_t *frame);
void rk_trace_sent(uint16_t command, const uint8_t *wire, int length, int queued);
void rk_trace_rejected(rk_rx_reject_reason_t reason, const tlv_frame_t *frame);
void rk_trace_progress(const tlv_frame_t *frame, int previous_valid, unsigned percent);
/* queried为0/1表示本轮未查询/已尝试应答，-1表示未绑定启动频率。 */
void rk_trace_ready(uint64_t sequence, int queried);
/* 每个索引输出一行，按 CH 编号列出应答中实际出现的通道。 */
void rk_trace_timing_values(uint8_t index, const rk_timing_readback_t *result);
void rk_trace_timing_unavailable(uint8_t index, const char *reason);
/* 全查结束时汇总收到有效应答的时序项数，通道数不参与计数。 */
void rk_trace_timing_summary(unsigned received, unsigned total);

/* 每项IDD结束只记一条窗口/采集质量，不逐点输出高频读数。 */
void rk_trace_idd_result(const idd_session_result_t *result);

#endif
