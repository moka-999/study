/* 仅由RK任务推进启动时序回读；跨任务的布防/放行入口见rk_task_control.h。 */
#ifndef RK_TIMING_READBACK_H
#define RK_TIMING_READBACK_H

#include "frame.h"

/* 自动和手动 0x43 查询共用此上限，单位 ms，从本帧进入 UART 发送队列起计时。 */
#define RK_TIMING_READBACK_TIMEOUT_MS 500U

/* 仅传已经入UART的心跳Seq和经过完整校验的普通心跳Seq。 */
void rk_timing_readback_heartbeat_tx(uint64_t seq);
void rk_timing_readback_heartbeat_rx(uint64_t seq);
/* 已发出查询时，后续成功发送的任一SPL帧都可成为查询应答的最近Seq。 */
void rk_timing_readback_note_queued(uint64_t seq);
/* 手动上电后PC也可能先写时序；已发出第一条设置时，等本批设置结束再开始读。 */
void rk_timing_readback_note_write(void);
/* 识别到0x43应答返回1；无关帧返回0，交给原协议处理。 */
int rk_timing_readback_receive(const tlv_frame_t *frame);
typedef enum { RK_READBACK_STARTUP, RK_READBACK_MANUAL } rk_readback_source_t;
/* RK 任务每轮推进指定来源，手动项可分别等待应答。返回 1 表示该来源正在处理。 */
int rk_timing_readback_poll(rk_readback_source_t source);
/* 用于确认协议口可以交给EDK，包括已经受理的手动查询。 */
int rk_timing_readback_manual_pending(void);
/* 仅RK任务调用；返回1表示刚写入一条调试时序，调用方须留出普通设置的20ms间隔。 */
int rk_timing_manual_write_poll(void);
/* 掉电时丢弃旧启动的回读记录；尚未发送首个心跳的新启动布防仍有效。 */
void rk_timing_readback_power_off(void);

#endif
