/* 启动扫描按设置顺序读回，手动查询按索引独立收发；二者都由 RK 任务写协议串口。 */
#include "rk_timing_readback.h"
#include "rk_task_control.h"
#include "handoff_mcu.h"
#include "pwr_ctrl.h"
#include "rk_link.h"
#include "rk_trace.h"
#include "task.h"
#include "uart_router.h"
#include <stdbool.h>
#include <string.h>

typedef enum {
    READ_IDLE, READ_WAIT_HEARTBEAT, READ_NEXT, READ_WAIT_REPLY
} read_phase_t;

static struct {
    read_phase_t phase;
    bool defer, heartbeat_sent, ready;
    uint8_t index;
    uint64_t first_heartbeat, last_heartbeat;
} s_startup;

/* 同一索引最多一帧在途。应答可带 RK 最近收到的 MCU Seq，靠回显索引区分交错的查询。 */
static struct {
    uint64_t first_seq;
    TickType_t sent_tick;
} s_query[RK_TIMING_INDEX_COUNT];
static uint64_t s_last_queued_seq;
static uint32_t s_manual, s_waiting, s_before_write;
static struct {
    uint16_t command; /* 0表示空闲；有效时序命令减0x25即0x43回读索引。 */
    bool sent;
    uint32_t value;
} s_set;
/* 全查只保存尚未完成的索引；单查或启动扫描收到同一项的应答也可以交付。 */
static uint32_t s_all_pending;
typedef struct { uint8_t received, total; } check_summary_t;
static check_summary_t s_all_summary;
_Static_assert(RK_TIMING_INDEX_COUNT <= 32U, "时序索引须能用一个位图表示");

static uint32_t index_bit(uint8_t index)
{
    return (uint32_t)1U << index;
}

int rk_timing_readback_manual_pending(void)
{
    taskENTER_CRITICAL();
    int pending = (s_manual | s_all_pending) != 0U || s_set.command != 0U;
    taskEXIT_CRITICAL();
    return pending;
}

/* 在临界区结算；有效应答每次只有一个索引，超时或取消可一次结束多项。 */
static check_summary_t finish_all_items(uint32_t indices, bool received)
{
    if (s_all_pending & indices) {
        if (received) ++s_all_summary.received;
        s_all_pending &= ~indices;
        if (!s_all_pending) return s_all_summary;
    }
    return (check_summary_t){0};
}

/* 调用方持有临界区；复位/换协议后不把上一轮应答交给新请求。 */
static uint32_t clear_queries(check_summary_t *summary)
{
    uint32_t manual = s_manual | s_all_pending;
    if (s_set.command) manual |= index_bit((uint8_t)(s_set.command - RK_CMD_SET_TIMING_BASE));
    s_set.command = 0U;
    s_startup.ready = false;
    *summary = finish_all_items(manual, false);
    s_manual = s_waiting = s_before_write = 0U;
    return manual;
}

static void trace_unavailable(uint32_t indices, const char *reason, check_summary_t summary)
{
    for (uint8_t i = 0U; indices; ++i, indices >>= 1U) {
        if (indices & 1U) rk_trace_timing_unavailable(i, reason);
    }
    if (summary.total) rk_trace_timing_summary(summary.received, summary.total);
}

void rk_timing_readback_arm(int defer_until_settings)
{
    check_summary_t summary;
    taskENTER_CRITICAL();
    uint32_t interrupted = clear_queries(&summary);
    s_startup.phase = READ_WAIT_HEARTBEAT;
    s_startup.defer = defer_until_settings != 0;
    s_startup.heartbeat_sent = false;
    s_startup.index = 0U;
    taskEXIT_CRITICAL();
    trace_unavailable(interrupted, "restart", summary);
}

const char *rk_task_check_timing(const char *name)
{
    bool all = name && strcmp(name, "all") == 0;
    uint32_t indices = 0U;
    uint8_t count = 0U;
    for (uint8_t index = 0; name && index < RK_TIMING_INDEX_COUNT; ++index) {
        const char *field = rk_timing_index_name(index);
        if (field && (all || strcmp(field, name) == 0)) {
            indices |= index_bit(index);
            ++count;
        }
    }
    if (!indices) return "invalidTiming";

    const char *error = NULL;
    taskENTER_CRITICAL();
    if (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) error = "powerOff";
    else if (rk_task_stopping()) error = "stopping";
    else if (!handoff_mcu_tlv_allowed()) error = "uartHandoff";
    else if (!all) s_manual |= indices;
    else if (!s_all_pending) {
        s_all_summary = (check_summary_t){.total = count};
        s_all_pending = indices;
    }
    taskEXIT_CRITICAL();
    return error;
}

int rk_task_set_pending(void)
{
    taskENTER_CRITICAL();
    int pending = s_set.command != 0U;
    taskEXIT_CRITICAL();
    return pending;
}

const char *rk_task_set_timing(uint16_t command, uint32_t value)
{
    if (!rk_is_timing_command(command)) return "invalidTiming";
    const char *error = NULL;
    taskENTER_CRITICAL();
    if (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) error = "powerOff";
    else if (rk_task_stopping() || s_set.command) error = "busy";
    else if (!handoff_mcu_tlv_allowed()) error = "uartHandoff";
    else if (!s_startup.ready) error = "notReady";
    /* 已经发出的同项查询对应写入前的值，等待它结束才接收设置。 */
    else if (s_waiting & index_bit((uint8_t)(command - RK_CMD_SET_TIMING_BASE))) error = "busy";
    else {
        s_set.command = command;
        s_set.value = value;
        s_set.sent = false;
    }
    taskEXIT_CRITICAL();
    return error;
}

int rk_timing_manual_write_poll(void)
{
    taskENTER_CRITICAL();
    uint16_t command = s_set.sent ? 0U : s_set.command;
    uint32_t value = s_set.value;
    taskEXIT_CRITICAL();
    if (!command) return 0;
    uint8_t index = (uint8_t)(command - RK_CMD_SET_TIMING_BASE);
    uint64_t seq = rk_link_next_sequence();
    uint8_t wire[TLV_TIMING_FRAME_SIZE];
    int length = tlv_pack_timing(command, value, seq, wire, sizeof(wire));
    int queued = length == (int)sizeof(wire) && uart_send(UART_PORT_RK_PROTOCOL, wire, sizeof(wire)) == 0;
    rk_trace_sent(command, wire, length, queued);
    taskENTER_CRITICAL();
    if (s_set.command == command) {
        if (queued) {
            s_set.sent = true;
            s_manual |= index_bit(index);
        } else s_set.command = 0U;
    }
    taskEXIT_CRITICAL();
    if (queued) rk_link_note_queued(seq);
    else rk_trace_timing_unavailable(index, "txFailed");
    return queued;
}

void rk_timing_readback_settings_done(void)
{
    taskENTER_CRITICAL();
    s_startup.defer = false;
    taskEXIT_CRITICAL();
}

void rk_timing_readback_note_write(void)
{
    taskENTER_CRITICAL();
    if (s_startup.phase == READ_WAIT_HEARTBEAT) s_startup.defer = true;
    /* 设置前发出的手动查询仍向操作者交付，自动扫描须重新查询设置后的值。 */
    s_before_write = s_waiting;
    taskEXIT_CRITICAL();
}

int rk_timing_startup_ready(void)
{
    taskENTER_CRITICAL();
    int ready = s_startup.ready;
    taskEXIT_CRITICAL();
    return ready;
}

int rk_timing_readback_busy(void)
{
    taskENTER_CRITICAL();
    int busy = !s_startup.defer && (s_startup.phase == READ_NEXT || s_startup.phase == READ_WAIT_REPLY);
    taskEXIT_CRITICAL();
    return busy;
}

void rk_timing_readback_cancel(void)
{
    check_summary_t summary;
    taskENTER_CRITICAL();
    uint32_t interrupted = clear_queries(&summary);
    s_startup.phase = READ_IDLE;
    taskEXIT_CRITICAL();
    trace_unavailable(interrupted, "cancelled", summary);
}

void rk_timing_readback_power_off(void)
{
    check_summary_t summary;
    taskENTER_CRITICAL();
    uint32_t interrupted = clear_queries(&summary);
    if (s_startup.phase != READ_WAIT_HEARTBEAT || s_startup.heartbeat_sent) s_startup.phase = READ_IDLE;
    taskEXIT_CRITICAL();
    trace_unavailable(interrupted, "powerOff", summary);
}

void rk_timing_readback_heartbeat_tx(uint64_t seq)
{
    taskENTER_CRITICAL();
    if (s_startup.phase == READ_WAIT_HEARTBEAT) {
        if (!s_startup.heartbeat_sent) s_startup.first_heartbeat = seq;
        s_startup.heartbeat_sent = true;
        s_startup.last_heartbeat = seq;
    }
    taskEXIT_CRITICAL();
}

void rk_timing_readback_note_queued(uint64_t seq)
{
    taskENTER_CRITICAL();
    s_last_queued_seq = seq;
    taskEXIT_CRITICAL();
}

static bool in_window(uint64_t seq, uint64_t first, uint64_t last)
{
    return seq - first <= last - first;
}

void rk_timing_readback_heartbeat_rx(uint64_t seq)
{
    taskENTER_CRITICAL();
    if (s_startup.phase == READ_WAIT_HEARTBEAT && s_startup.heartbeat_sent &&
        in_window(seq, s_startup.first_heartbeat, s_startup.last_heartbeat)) {
        s_startup.ready = true;
        s_startup.phase = READ_NEXT;
    }
    taskEXIT_CRITICAL();
}

/* 调用方持有临界区。仅结束这个索引；当前自动扫描也在等它时才推进扫描。 */
static void finish_index(uint8_t index)
{
    uint32_t bit = index_bit(index);
    s_manual &= ~bit;
    s_waiting &= ~bit;
    s_before_write &= ~bit;
    if (s_set.sent && s_set.command == RK_CMD_SET_TIMING_BASE + index) s_set.command = 0U;
    if (s_startup.phase == READ_WAIT_REPLY && s_startup.index == index) {
        do {
            ++s_startup.index;
        } while (s_startup.index < RK_TIMING_INDEX_COUNT && !rk_timing_index_name(s_startup.index));
        s_startup.phase = s_startup.index < RK_TIMING_INDEX_COUNT ? READ_NEXT : READ_IDLE;
    }
}

static bool expired(uint8_t index, TickType_t now)
{
    return (TickType_t)(now - s_query[index].sent_tick) >= pdMS_TO_TICKS(RK_TIMING_READBACK_TIMEOUT_MS);
}

int rk_timing_readback_receive(const tlv_frame_t *frame)
{
    bool timing_reply = false;
    uint8_t index = RK_TIMING_INDEX_COUNT;
    for (unsigned i = 0U; i < frame->count; ++i) {
        const tlv_unit_t *unit = &frame->units[i];
        const uint8_t *data = tlv_unit_get_data(frame, unit);
        if (unit->type == TLV_TYPE_TIMING_INDEX && unit->data_len == 1U) index = data[0];
        if (unit->type == TLV_TYPE_RESULT_CMD && unit->data_len == sizeof(uint16_t) &&
            (data[0] | ((uint16_t)data[1] << 8)) == RK_CMD_QUERY_TIMING) timing_reply = true;
    }
    if (!timing_reply) return 0;
    rk_timing_readback_t result;
    if (rk_timing_readback_decode(frame, index, &result) == 0) {
        check_summary_t summary = {0};
        taskENTER_CRITICAL();
        bool matched = (s_waiting & index_bit(index)) &&
            in_window(frame->sequence, s_query[index].first_seq, s_last_queued_seq) &&
            !expired(index, xTaskGetTickCount());
        if (matched) {
            finish_index(index);
            summary = finish_all_items(index_bit(index), true);
        }
        taskEXIT_CRITICAL();
        if (matched) rk_trace_timing_values(index, &result);
        if (summary.total) rk_trace_timing_summary(summary.received, summary.total);
    }
    return 1; /* 过期或格式错误的 0x43 仍由回读模块消费。 */
}

int rk_timing_readback_poll(rk_readback_source_t source)
{
    uint8_t index = RK_TIMING_INDEX_COUNT;
    uint32_t timed_out = 0U;
    taskENTER_CRITICAL();
    if (s_set.command && !s_set.sent) {
        taskEXIT_CRITICAL();
        return 0;
    }
    TickType_t now = xTaskGetTickCount();
    if (source == RK_READBACK_MANUAL) {
        /* 取全查位图的最低有效位，每项结束才推进；显式单查仍可独立发出。 */
        uint32_t next_in_batch = s_all_pending & (0U - s_all_pending);
        uint32_t requested = s_manual | next_in_batch;
        if (!requested) {
            taskEXIT_CRITICAL();
            return 0;
        }
        /* 一轮最多新增一帧，同时检查所有在途手动项，避免慢应答挡住后来的查询。 */
        for (uint8_t i = 0U; i < RK_TIMING_INDEX_COUNT; ++i) {
            uint32_t bit = index_bit(i);
            if (!(requested & bit)) continue;
            if (s_waiting & bit) {
                if (expired(i, now)) {
                    timed_out |= bit;
                    finish_index(i);
                }
            } else if (index == RK_TIMING_INDEX_COUNT) {
                index = i;
            }
        }
    } else {
        if (s_startup.defer || (s_startup.phase != READ_NEXT && s_startup.phase != READ_WAIT_REPLY)) {
            taskEXIT_CRITICAL();
            return 0;
        }
        index = s_startup.index;
        uint32_t bit = index_bit(index);
        if (!(s_before_write & bit)) s_startup.phase = READ_WAIT_REPLY;
        if (s_waiting & bit) {
            if (expired(index, now)) {
                timed_out = bit;
                finish_index(index);
            }
            index = RK_TIMING_INDEX_COUNT;
        }
    }
    check_summary_t summary = finish_all_items(timed_out, false);
    uint64_t seq = 0U;
    if (index < RK_TIMING_INDEX_COUNT) {
        seq = rk_link_next_sequence();
        s_waiting |= index_bit(index);
        s_query[index].first_seq = seq;
    }
    taskEXIT_CRITICAL();

    trace_unavailable(timed_out, "timeout", summary);
    if (index == RK_TIMING_INDEX_COUNT) return 1;

    uint8_t wire[TLV_TIMING_QUERY_FRAME_SIZE];
    int length = tlv_pack_timing_query(index, seq, wire, sizeof(wire));
    int queued = length == (int)sizeof(wire) && uart_send(UART_PORT_RK_PROTOCOL, wire, sizeof(wire)) == 0;
    taskENTER_CRITICAL();
    bool current = (s_waiting & index_bit(index)) && s_query[index].first_seq == seq;
    if (current) {
        if (queued) s_query[index].sent_tick = xTaskGetTickCount();
        else {
            finish_index(index);
            summary = finish_all_items(index_bit(index), false);
        }
    }
    taskEXIT_CRITICAL();
    if (queued) rk_link_note_queued(seq);
    rk_trace_sent(RK_CMD_QUERY_TIMING, wire, length, queued);
    if (current && !queued) trace_unavailable(index_bit(index), "txFailed", summary);
    return 1;
}
