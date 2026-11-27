/* B1 RK线帧观察。按线上顺序显示TLV，所有进度和同类拒绝的限流规则集中在本文件。 */
#include "rk_trace.h"
#include "protocol_trace.h"
#include "log_console.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

/* 最长正式7个TLV可完整显示。未知扩展超限保留完整单元并标记省略。 */
#define RK_TRACE_TLV_TEXT_SIZE 176U
#define RK_TRACE_REJECT_INTERVAL_MS 1000U
#define RK_TRACE_PROGRESS_STEP 25U
static const char s_hex[] = "0123456789ABCDEF";

static void log_frame(const char *tag, const tlv_frame_t *frame, const char *outcome)
{
    if (!log_console_mcu_selected()) return;
    char values[RK_TRACE_TLV_TEXT_SIZE] = "";
    size_t used = 0;
    for (unsigned i = 0; i < frame->count; ++i) {
        const tlv_unit_t *unit = &frame->units[i];
        /* 为超长扩展保留省略标志，绝不把截断的Value当成完整数据。 */
        size_t space = sizeof(values) - used - sizeof(" ...");
        int n = snprintf(values + used, space, " %04X[%u]=", unit->type, unit->data_len);
        if (n < 0 || (size_t)n + 2U * unit->data_len >= space) {
            memcpy(values + used, " ...", sizeof(" ..."));
            break;
        }
        used += (size_t)n;
        const uint8_t *data = tlv_unit_get_data(frame, unit);
        for (unsigned j = 0; j < unit->data_len; ++j) {
            values[used++] = s_hex[data[j] >> 4];
            values[used++] = s_hex[data[j] & 0x0FU];
        }
        values[used] = '\0';
    }
    char seq[TRACE_U64_TEXT_SIZE];
    trace_line(tag, "seq=%s%s%s", trace_u64(frame->sequence, seq), values, outcome);
}

void rk_trace_received(const tlv_frame_t *frame) { log_frame("RK RX", frame, ""); }

void rk_trace_query_while_busy(const tlv_frame_t *frame)
{
    log_frame("RK RX", frame, " ignored=algorithmBusy");
}

void rk_trace_sent(uint16_t command, const uint8_t *wire, int length, int queued)
{
    if (!log_console_mcu_selected()) return;
    tlv_frame_t frame;
    if (length > 0 && length <= (int)TLV_FRAME_MAX && tlv_unpack(wire, (uint16_t)length, &frame) >= 0)
        log_frame("RK TX", &frame, queued ? " uart=queued" : " uart=failed");
    else trace_line("RK TX", "cmd=%04X buildFailed", command);
}

void rk_trace_rejected(rk_rx_reject_reason_t reason, const tlv_frame_t *frame)
{
    static const char *const names[RK_RX_REJECT_COUNT] = {
        [RK_RX_REJECT_NO_PENDING] = "noPending", [RK_RX_REJECT_SEQUENCE] = "seqWindow",
        [RK_RX_REJECT_ALGORITHM] = "algorithm", [RK_RX_REJECT_PROGRESS] = "progressFormat",
        [RK_RX_REJECT_RESULT] = "resultFormat", [RK_RX_REJECT_TLV] = "invalidTLV"
    };
    static uint32_t last_ms[RK_RX_REJECT_COUNT];
    static uint8_t seen[RK_RX_REJECT_COUNT];
    if (!log_console_mcu_selected() || reason <= RK_RX_REJECT_NONE || reason >= RK_RX_REJECT_COUNT) return;
    uint32_t now = HAL_GetTick();
    if (seen[reason] && now - last_ms[reason] < RK_TRACE_REJECT_INTERVAL_MS) return;
    seen[reason] = 1U;
    last_ms[reason] = now;
    if (frame) log_frame("RK RX", frame, " rejected");
    rk_rx_diagnostics_t d;
    if (rk_link_get_rx_diagnostics(&d) != 0) return;
    if (!d.waiting) {
        trace_line("RK RX", "reject=%s count=%lu", names[reason], (unsigned long)d.counts[reason]);
    } else {
        char first[TRACE_U64_TEXT_SIZE], last[TRACE_U64_TEXT_SIZE];
        trace_line("RK RX", "reject=%s count=%lu expected=%04X seqWindow=%s..%s",
                   names[reason], (unsigned long)d.counts[reason], d.expected_command,
                   trace_u64(d.command_sequence, first), trace_u64(d.last_tx_sequence, last));
    }
}

void rk_trace_progress(const tlv_frame_t *frame, int previous_valid, unsigned percent)
{
    /* 只由RK任务调用；首次进度重置门槛，后续倒退或跨档抖动不重复刷屏。 */
    static unsigned next_percent;
    if (previous_valid && percent < next_percent) return;
    next_percent = (percent / RK_TRACE_PROGRESS_STEP + 1U) * RK_TRACE_PROGRESS_STEP;
    log_frame("RK RX", frame, " progress");
}

void rk_trace_ready(uint64_t sequence, int queried)
{
    char seq[TRACE_U64_TEXT_SIZE];
    trace_line("RK RX", "SPL ready seq=%s freqQuery=%s", trace_u64(sequence, seq),
               queried < 0 ? "unbound" : queried ? "1" : "0");
}

void rk_trace_timing_values(uint8_t index, const rk_timing_readback_t *result)
{
    if (!log_console_mcu_selected()) return;
    char channels[RK_TIMING_CHANNEL_COUNT * (sizeof(" CH3=4294967295") - 1U) + 1U] = "";
    size_t used = 0;
    for (unsigned ch = 0; ch < RK_TIMING_CHANNEL_COUNT; ++ch) {
        if (!(result->present & (1U << ch))) continue;
        size_t space = sizeof(channels) - used;
        int n = snprintf(channels + used, space, " CH%u=%lu", ch, (unsigned long)result->values[ch]);
        if (n < 0 || (size_t)n >= space) return;
        used += (size_t)n;
    }
    trace_line("RK TIMING", "index=%u %s%s", index, rk_timing_index_name(index), channels);
}

void rk_trace_timing_unavailable(uint8_t index, const char *reason)
{
    trace_line("RK TIMING", "index=%u %s unavailable=%s", index, rk_timing_index_name(index), reason);
}

void rk_trace_timing_summary(unsigned received, unsigned total)
{
    trace_line("RK TIMING", "check all received=%u/%u", received, total);
}

void rk_trace_idd_result(const idd_session_result_t *result)
{
    if (result == NULL) return;
    trace_line("IDD", "cmd=%04X windowMs=%lu samples=%lu flags=%02lX maxGapMs=%lu",
               result->command, (unsigned long)result->collected_ms,
               (unsigned long)result->stored_samples, (unsigned long)result->flags,
               (unsigned long)result->max_gap_ms);
}
