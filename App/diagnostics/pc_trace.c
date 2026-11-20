/* B1 PC业务和TCP收发记录。这里决定显示哪些字段，协议任务仍拥有连接和发送状态。 */
#include "pc_trace.h"
#include "pc_command.h"
#include "pc_test_message.h"
#include "pc_power_service.h"
#include "frame.h"
#include "ddr_power.h"
#include "protocol_trace.h"
#include "log_console.h"
#include "board_rk_config.h"
#include <stdio.h>

static uint32_t s_connection;
static char s_peer_ip[sizeof("255.255.255.255")];
static uint16_t s_peer_port;

/* 只折叠方向正确的心跳和采样。0006以原请求号归入相应错误计数。 */
static trace_group_t periodic_request(uint16_t command)
{
    switch (command) {
    case PC_COMMAND_CLIENT_HEARTBEAT:     return TRACE_PC_HEARTBEAT;
    case PC_COMMAND_QUERY_POWER_MONITOR:  return TRACE_PC_SAMPLE;
    default:                             return TRACE_GROUP_COUNT;
    }
}

void pc_trace_connected(const uint8_t peer_ip[4], uint16_t peer_port)
{
    ++s_connection;
    snprintf(s_peer_ip, sizeof(s_peer_ip), "%u.%u.%u.%u",
             peer_ip[0], peer_ip[1], peer_ip[2], peer_ip[3]);
    s_peer_port = peer_port;
    trace_line("PC", "conn=%lu connected peer=%s:%u listener=closed",
               (unsigned long)s_connection, s_peer_ip, s_peer_port);
}

void pc_trace_closed(const char *reason)
{
    trace_line("PC", "conn=%lu closed reason=%s peer=%s:%u",
               (unsigned long)s_connection, reason, s_peer_ip, s_peer_port);
}

void pc_trace_rx_error(int socket_error)
{
    trace_line("PC", "conn=%lu recvFailed errno=%d peer=%s:%u",
               (unsigned long)s_connection, socket_error, s_peer_ip, s_peer_port);
}

void pc_trace_rx(const pc_protocol_frame_t *frame)
{
    trace_group_t group = periodic_request(frame->command_id);
    if (group != TRACE_GROUP_COUNT) trace_count(group, TRACE_RX);
    else trace_line("PC RX", "conn=%lu cmd=%04X seq=%lu len=%lu", (unsigned long)s_connection,
                    frame->command_id, (unsigned long)frame->sequence, (unsigned long)frame->length);
}

void pc_trace_tx(const pc_dispatch_response_t *r, uint32_t seq, size_t queued, int socket_error)
{
    uint16_t cmd = r->command_id;
    int failed = queued != r->length;
    trace_group_t group = TRACE_GROUP_COUNT;
    if (cmd == PC_COMMAND_SERVER_HEARTBEAT) group = TRACE_PC_HEARTBEAT;
    else if (cmd == PC_COMMAND_POWER_MONITOR_RESP) group = TRACE_PC_SAMPLE;
    else if (cmd == PC_COMMAND_COMMAND_RESP) group = periodic_request(r->request_command);
    if (group != TRACE_GROUP_COUNT) {
        trace_count(group, failed || cmd == PC_COMMAND_COMMAND_RESP ? TRACE_ERROR : TRACE_TX);
        if (!failed) return;
    }
    if (cmd == PC_COMMAND_TEST_RESULT_REPORT || cmd == PC_COMMAND_PARA_RESULT) {
        char error[sizeof(" sendFailed errno=-2147483648")] = "";
        if (failed) snprintf(error, sizeof(error), " sendFailed errno=%d", socket_error);
        trace_line("PC TX", "conn=%lu cmd=%04X idx=%lu seq=%lu len=%lu tcpQueued=%lu%s",
                   (unsigned long)s_connection, cmd, (unsigned long)r->result_index, (unsigned long)seq,
                   (unsigned long)r->length, (unsigned long)queued, error);
    } else if (failed) {
        trace_line("PC TX", "conn=%lu cmd=%04X seq=%lu len=%lu tcpQueued=%lu sendFailed errno=%d",
                   (unsigned long)s_connection, cmd, (unsigned long)seq, (unsigned long)r->length,
                   (unsigned long)queued, socket_error);
    } else if (cmd == PC_COMMAND_COMMAND_RESP) {
        /* 0006只含MCU生成的状态和错误码。用实际正文长度打印，无须再次解析JSON。 */
        trace_line("PC TX", "conn=%lu cmd=%04X seq=%lu len=%lu req=%04X %.*s",
                   (unsigned long)s_connection, cmd, (unsigned long)seq, (unsigned long)r->length, r->request_command,
                   (int)(r->length - PC_PROTOCOL_HEADER_SIZE), (const char *)r->data + PC_PROTOCOL_HEADER_SIZE);
    } else {
        trace_line("PC TX", "conn=%lu cmd=%04X seq=%lu tcpQueued=%lu", (unsigned long)s_connection,
                   cmd, (unsigned long)seq, (unsigned long)queued);
    }
}

void pc_trace_start(uint16_t cmd, const pc_test_start_view_t *start, const char *name, uint32_t cookie)
{
    char freq[TRACE_U64_TEXT_SIZE], index[TRACE_U64_TEXT_SIZE];
    if (!log_console_mcu_selected()) return;
    int64_t frequency = start->timing.frequency;
    /* 先转无符号再取绝对值，INT64_MIN也可完整打印；不推断PC所用频率单位。 */
    const char *value = trace_u64(frequency < 0 ? 0U - (uint64_t)frequency : (uint64_t)frequency, freq);
    const char *idx = cmd == PC_COMMAND_SLT_START ? "null" : trace_u64(start->algorithm_index, index);
    unsigned count = (unsigned)__builtin_popcount(start->timing.present);
    trace_line("PC RX", "cmd=%04X type=%s test=%s idx=%s cookie=%lu reboot=%u boot=%s Freq=%s%s timingCount=%u",
               cmd, start->algorithm_type, name, idx, (unsigned long)cookie, start->reboot,
               start->external_ready ? "external" : start->boot_mode, frequency < 0 ? "-" : "", value, count);
    trace_line("RK CONFIG", "cookie=%lu timingEnable=%u freqEnable=%u",
               (unsigned long)cookie, (unsigned)B1_RK_TIMING_ENABLE, (unsigned)B1_RK_FREQ_ENABLE);
    if (!start->external_ready)
        trace_line("PC RX", "cookie=%lu power=%s target_mV=%u/%u/%u", (unsigned long)cookie,
                   start->memory_type, start->target_mv[DDR_RAIL_VDD1], start->target_mv[DDR_RAIL_VDD2],
                   start->target_mv[DDR_RAIL_VDDQ]);
}

void pc_trace_result(const char *name, uint32_t index, const rk_final_result_t *result)
{
    if (!log_console_mcu_selected()) return;
    char dies[RK_RESULT_DIE_COUNT][sizeof("FAIL@0xFFFFFFFFFFFFFFFF")] = {"PASS", "PASS"};
    for (unsigned i = 0; i < result->failure_count; ++i) {
        const rk_failure_detail_t *f = &result->failures[i];
        snprintf(dies[f->die], sizeof(dies[0]), "FAIL@0x%08lX%08lX",
                 (unsigned long)(f->error_address >> 32), (unsigned long)(uint32_t)f->error_address);
    }
    char ms[TRACE_U64_TEXT_SIZE];
    trace_line("RESULT", "%s idx=%lu time=%sms die0=%s die1=%s", name, (unsigned long)index,
               trace_u64(result->elapsed_ms, ms), dies[0], dies[1]);
}

void pc_trace_discard(uint32_t cookie, const char *reason)
{
    trace_line("PC", "cookie=%lu discard pending result reason=%s", (unsigned long)cookie, reason);
}

void pc_trace_power_phase(const pc_power_diagnostic_t *d)
{
    static const char *const phases[] = {
        [PC_POWER_IDLE] = "idle", [PC_POWER_REQUEST_OFF] = "requestOff", [PC_POWER_WAIT_OFF] = "waitOff",
        [PC_POWER_OFF_HOLD] = "offHold", [PC_POWER_BOOT_SETTLE] = "bootSettle", [PC_POWER_WAIT_ON] = "waitOn",
        [PC_POWER_WAIT_RK_READY] = "waitRkReady", [PC_POWER_DONE] = "done"
    };
    if (!log_console_mcu_selected()) return;
    char line[LOG_CONSOLE_RECORD_SIZE];
    int length = snprintf(line, sizeof(line), "[power] action=%u phase=%s ms=%lu error=%s at=%s\r\n",
        (unsigned)d->action, phases[d->phase], (unsigned long)d->phase_since_ms,
        d->error ? d->error : "none", phases[d->failed_phase]);
    if (length > 0 && (size_t)length < sizeof(line)) log_console_write_record(line, (size_t)length);
}
