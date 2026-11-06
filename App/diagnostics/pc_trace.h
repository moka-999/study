#ifndef B1_PC_TRACE_H
#define B1_PC_TRACE_H

#include "pc_dispatch.h"
#include <stddef.h>

/* 只借用业务视图；TCP任务包含本头时无需引入JSON、RK编解码或板级驱动。 */
struct pc_test_start_view;
struct pc_power_diagnostic;
struct rk_final_result;

/* 仅TCP任务通知；连接号按accept递增，两个方向的Seq分别使用各自线上值。 */
void pc_trace_connected(void);
void pc_trace_closed(const char *reason);
void pc_trace_rx(const pc_protocol_frame_t *frame);
void pc_trace_tx(const pc_dispatch_response_t *response, uint32_t seq, size_t queued, int socket_error);
/* 配置参数须已通过校验，name取命令表的规范名称。结果只在首次领取时打印。 */
void pc_trace_start(uint16_t command, const struct pc_test_start_view *start, const char *name, uint32_t cookie);
void pc_trace_result(const char *name, uint32_t index, const struct rk_final_result *result);
void pc_trace_discard(uint32_t cookie, const char *reason);
/* 在业务层已更新时间后借用电源阶段快照，沿用已有[power]行格式。 */
void pc_trace_power_phase(const struct pc_power_diagnostic *diagnostic);

#endif
