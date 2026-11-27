#ifndef B1_PC_TRACE_H
#define B1_PC_TRACE_H

#include "pc_dispatch.h"
#include <stddef.h>

/* 只借用业务视图；TCP任务包含本头时无需引入JSON、RK编解码或板级驱动。 */
struct pc_test_start_view;
struct pc_power_diagnostic;
struct rk_final_result;

/* 仅TCP任务通知；连接号按accept递增，两个方向的Seq分别使用各自线上值。 */
/* peer_ip为网络顺序的四字节IPv4地址，peer_port为主机顺序端口。 */
void pc_trace_connected(const uint8_t peer_ip[4], uint16_t peer_port);
void pc_trace_closed(const char *reason);
/* 监听等待失败时尚无对端，单独记录select返回的lwIP错误码。 */
void pc_trace_listen_error(int socket_error);
/* recv失败时立即复制lwIP错误码，避免后续调用改写errno。 */
void pc_trace_rx_error(int socket_error);
void pc_trace_rx(const pc_protocol_frame_t *frame);
void pc_trace_tx(const pc_dispatch_response_t *response, uint32_t seq, size_t queued, int socket_error);
/* 配置参数须已通过校验，name取命令表的规范名称。结果只在首次领取时打印。 */
void pc_trace_start(uint16_t command, const struct pc_test_start_view *start, const char *name, uint32_t cookie);
/* 格式化已解析的RK结果，含毫秒耗时、两颗判定及原始失败地址，供业务日志和调试端共用。 */
void pc_trace_format_result(const struct rk_final_result *result, char *text, size_t capacity);
void pc_trace_result(const char *name, uint32_t index, const struct rk_final_result *result);
void pc_trace_discard(uint32_t cookie, const char *reason);
/* 在业务层已更新时间后借用电源阶段快照，沿用已有[power]行格式。 */
void pc_trace_power_phase(const struct pc_power_diagnostic *diagnostic);

#endif
