#include "idd_session.h"
#include <limits.h>
#include <string.h>

_Static_assert(sizeof(idd_session_point_t) == 16U, "逐点缓冲大小必须稳定");
_Static_assert(IDD_SESSION_WINDOW_MS <= UINT16_MAX, "样点偏移必须可用u16毫秒表示");

static void add_saturated(uint32_t *value, uint32_t count)
{
    *value = count > UINT32_MAX - *value ? UINT32_MAX : *value + count;
}

/* 只处理本次已保存点，计算完成即释放逐点缓冲的借用；不在长临界区调用。 */
static void freeze(idd_session_t *session, uint32_t now_ms)
{
    if (session->frozen) return;
    idd_session_result_t *result = &session->result;
    if (session->phase == IDD_SESSION_CAPTURING) {
        uint32_t elapsed = now_ms - result->started_ms;
        result->collected_ms = elapsed <= INT32_MAX ? elapsed : 0U;
        if (result->collected_ms > IDD_SESSION_WINDOW_MS)
            result->collected_ms = IDD_SESSION_WINDOW_MS;
        /* 清理事件携带旧时间时也不能把已保存样点之后的覆盖长度回退。 */
        if (result->collected_ms < session->last_offset_ms)
            result->collected_ms = session->last_offset_ms;
    }
    uint32_t tail = result->collected_ms - session->last_offset_ms;
    if (tail > result->max_gap_ms) result->max_gap_ms = tail;
    for (uint32_t n = 0U; n < result->stored_samples; ++n) {
        const idd_session_point_t *point = &session->points[n];
        for (unsigned rail = 0U; rail < IDD_SESSION_RAILS; ++rail) {
            idd_session_metric_t *metric = &result->current[rail];
            if (!(point->valid_mask & (1U << rail))) {
                ++metric->failed_count;
                result->flags |= IDD_SESSION_READ_ERROR;
                continue;
            }
            uint32_t value = point->current_ua[rail];
            if (!metric->valid_count || value < metric->minimum_ua) metric->minimum_ua = value;
            if (!metric->valid_count || value > metric->maximum_ua) metric->maximum_ua = value;
            metric->sum_ua += value; /* 至多5120个u32，u64累加不会溢出。 */
            ++metric->valid_count;
        }
    }
    for (unsigned rail = 0U; rail < IDD_SESSION_RAILS; ++rail) {
        idd_session_metric_t *metric = &result->current[rail];
        if (metric->valid_count) metric->mean_ua = (uint32_t)(metric->sum_ua / metric->valid_count);
        else result->flags |= IDD_SESSION_NO_DATA;
    }
    session->points = NULL;
    session->capacity = 0U;
    session->frozen = 1U;
}

int idd_session_begin(idd_session_t *session, uint16_t command,
                      idd_session_point_t *points, uint32_t capacity)
{
    if (!session || session->phase != IDD_SESSION_IDLE || command < 21U || command > 36U ||
        !points || !capacity || capacity > IDD_SESSION_CAPACITY) return -1;
    memset(session, 0, sizeof(*session));
    session->points = points;
    session->capacity = capacity;
    session->result.command = command;
    session->phase = IDD_SESSION_ENTER_READY;
    return 0;
}

int idd_session_next_frame(idd_session_t *session, uint64_t sequence,
                           uint8_t *wire, uint16_t capacity)
{
    if (!session || (session->phase != IDD_SESSION_ENTER_READY &&
                     session->phase != IDD_SESSION_EXIT_READY)) return 0;
    uint8_t action = session->phase == IDD_SESSION_ENTER_READY;
    int length = tlv_pack_idd_action(session->result.command, action, sequence, wire, capacity);
    if (length == TLV_IDD_ACTION_FRAME_SIZE) {
        session->phase = action ? IDD_SESSION_ENTER_TX : IDD_SESSION_EXIT_TX;
        if (action) session->result.enter_sequence = sequence;
        else session->result.exit_sequence = sequence;
    }
    return length;
}

static int matching_tx(const idd_session_t *session, uint64_t sequence)
{
    return session && ((session->phase == IDD_SESSION_ENTER_TX && session->result.enter_sequence == sequence) ||
                       (session->phase == IDD_SESSION_EXIT_TX && session->result.exit_sequence == sequence));
}

int idd_session_tx_complete(idd_session_t *session, uint64_t sequence, uint32_t now_ms)
{
    if (!matching_tx(session, sequence)) return -1;
    if (session->phase == IDD_SESSION_ENTER_TX) {
        session->result.started_ms = now_ms + IDD_SESSION_SETTLE_MS;
        session->phase = IDD_SESSION_SETTLING;
        if (session->result.flags & IDD_SESSION_CANCELLED) {
            freeze(session, now_ms);
            session->phase = IDD_SESSION_EXIT_READY;
        }
        return 0;
    }
    session->phase = IDD_SESSION_COMPLETE;
    return 0;
}

void idd_session_tx_failed(idd_session_t *session, uint64_t sequence, uint32_t now_ms)
{
    if (!matching_tx(session, sequence)) return;
    session->result.flags |= IDD_SESSION_TX_ERROR;
    freeze(session, now_ms);
    session->phase = IDD_SESSION_TX_FAULT;
}

void idd_session_poll(idd_session_t *session, uint32_t now_ms)
{
    if (!session) return;
    uint32_t elapsed = now_ms - session->result.started_ms;
    if (elapsed > INT32_MAX) return; /* 尚未到建立截止点，或调用方给了旧时间。 */
    if (session->phase == IDD_SESSION_SETTLING) session->phase = IDD_SESSION_CAPTURING;
    if (session->phase == IDD_SESSION_CAPTURING && elapsed >= IDD_SESSION_WINDOW_MS) {
        freeze(session, now_ms);
        session->phase = IDD_SESSION_EXIT_READY;
    }
}

int idd_session_feed(idd_session_t *session, uint32_t sample_sequence,
                     uint32_t begin_ms, uint32_t end_ms,
                     const uint32_t current_ua[IDD_SESSION_RAILS], uint8_t valid_mask)
{
    if (!session || (valid_mask & ~IDD_SESSION_VALID_ALL) || (valid_mask && !current_ua)) return -1;
    if (session->phase != IDD_SESSION_SETTLING && session->phase != IDD_SESSION_CAPTURING) return 0;
    idd_session_result_t *result = &session->result;
    uint32_t begin = begin_ms - result->started_ms;
    uint32_t end = end_ms - result->started_ms;
    if (begin >= IDD_SESSION_WINDOW_MS || end >= IDD_SESSION_WINDOW_MS || end < begin) {
        add_saturated(&result->boundary_samples, 1U);
        idd_session_poll(session, end_ms);
        return 0;
    }
    idd_session_poll(session, end_ms);
    if (result->stored_samples) {
        uint32_t step = sample_sequence - session->last_sample_sequence;
        if (!step || step > INT32_MAX || end < session->last_offset_ms) {
            add_saturated(&result->rejected_samples, 1U);
            result->flags |= IDD_SESSION_SAMPLE_ORDER;
            return 0;
        }
        if (step > 1U) {
            add_saturated(&result->missing_samples, step - 1U);
            result->flags |= IDD_SESSION_SAMPLE_GAP;
        }
    }
    if (result->stored_samples == session->capacity) {
        result->flags |= IDD_SESSION_OVERFLOW;
        freeze(session, end_ms);
        session->phase = IDD_SESSION_EXIT_READY;
        return 0;
    }
    idd_session_point_t *point = &session->points[result->stored_samples++];
    for (unsigned rail = 0U; rail < IDD_SESSION_RAILS; ++rail)
        point->current_ua[rail] = (valid_mask & (1U << rail)) ? current_ua[rail] : 0U;
    point->offset_ms = (uint16_t)end;
    point->valid_mask = valid_mask;
    point->reserved = 0U;
    uint32_t gap = end - session->last_offset_ms;
    if (gap > result->max_gap_ms) result->max_gap_ms = gap;
    session->last_offset_ms = (uint16_t)end;
    session->last_sample_sequence = sample_sequence;
    return 1;
}

void idd_session_cancel(idd_session_t *session, uint32_t now_ms)
{
    if (!session || session->phase == IDD_SESSION_IDLE || session->phase >= IDD_SESSION_COMPLETE) return;
    session->result.flags |= IDD_SESSION_CANCELLED;
    if (session->phase == IDD_SESSION_ENTER_TX) return;
    if (session->phase == IDD_SESSION_EXIT_TX) return;
    idd_session_poll(session, now_ms);
    freeze(session, now_ms);
    session->phase = session->phase == IDD_SESSION_ENTER_READY ?
                     IDD_SESSION_COMPLETE : IDD_SESSION_EXIT_READY;
}

void idd_session_power_off(idd_session_t *session, uint32_t now_ms)
{
    if (!session || session->phase == IDD_SESSION_IDLE || session->phase == IDD_SESSION_COMPLETE) return;
    session->result.flags |= IDD_SESSION_POWER_LOST;
    idd_session_poll(session, now_ms);
    freeze(session, now_ms);
    session->phase = session->result_taken ? IDD_SESSION_IDLE : IDD_SESSION_COMPLETE;
}

int idd_session_take_result(idd_session_t *session, idd_session_result_t *out)
{
    if (!session || !out || session->result_taken ||
        (session->phase != IDD_SESSION_COMPLETE && session->phase != IDD_SESSION_TX_FAULT)) return 0;
    *out = session->result;
    session->result_taken = 1U;
    if (session->phase == IDD_SESSION_COMPLETE) session->phase = IDD_SESSION_IDLE;
    return 1;
}
