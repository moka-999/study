#ifndef B1_PROTOCOL_TRACE_H
#define B1_PROTOCOL_TRACE_H

#include <stdint.h>

/* 调试输出只从任务上下文调用；失败只丢日志。数值文本保留完整u64十进制精度。 */
#define TRACE_U64_TEXT_SIZE sizeof("18446744073709551615")
const char *trace_u64(uint64_t value, char buffer[TRACE_U64_TEXT_SIZE]);
void trace_line(const char *tag, const char *format, ...) __attribute__((format(printf, 2, 3)));

/* 高频流量按方向累计，接收和发送均以MCU为视角。status只读取开机以来的快照。 */
typedef enum { TRACE_PC_HEARTBEAT, TRACE_PC_SAMPLE, TRACE_RK_HEARTBEAT, TRACE_GROUP_COUNT } trace_group_t;
typedef enum { TRACE_RX, TRACE_TX, TRACE_ERROR } trace_direction_t;
void trace_count(trace_group_t group, trace_direction_t direction);
void trace_report(void (*append)(const char *, ...));

#endif
