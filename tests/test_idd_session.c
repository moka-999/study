/* 运行正式IDD内部流程；仅模拟发送完成和采样事件，不调用UART/传感器/电源。 */
#include "idd_session.h"
#include <limits.h>
#include <string.h>

#define TEXT1(x) #x
#define TEXT(x) TEXT1(x)
#define CHECK(x) do { if (!(x)) fail("FAIL " TEXT(__LINE__) ": " #x "\n"); } while (0)

static idd_session_point_t points[IDD_SESSION_CAPACITY];
static const uint32_t currents[] = {100U, 200U, 300U};

static void message(const char *text)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = text;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}
static _Noreturn void finish(uint32_t code)
{
    const uint32_t block[] = {0x20026U, code};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register const uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) { }
}
static _Noreturn void fail(const char *text) { message(text); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }

static void frame(idd_session_t *session, uint64_t seq, uint8_t action)
{
    uint8_t wire[TLV_IDD_ACTION_FRAME_SIZE];
    CHECK(idd_session_next_frame(session, seq, wire, sizeof(wire) - 1U) == 0);
    CHECK(idd_session_next_frame(session, seq, wire, sizeof(wire)) == sizeof(wire));
    CHECK(wire[13] == 0x50U && wire[14] == 0U && wire[15] == 0U && wire[16] == 2U);
    CHECK(wire[17] == session->result.command && wire[18] == 0U);
    CHECK(wire[19] == 0x10U && wire[20] == 0x1AU && wire[21] == 0U && wire[22] == 1U);
    CHECK(wire[23] == action);
    tlv_frame_t parsed;
    CHECK(tlv_unpack(wire, sizeof(wire), &parsed) == 2);
    CHECK(parsed.sequence == seq);
    CHECK(idd_session_next_frame(session, seq + 1U, wire, sizeof(wire)) == 0);
}

static void enter(idd_session_t *session, uint16_t cmd, uint32_t now, uint32_t capacity)
{
    CHECK(idd_session_begin(session, cmd, points, capacity) == 0);
    frame(session, UINT64_C(0x1020304050607080), 1U);
    /* 入TX态后无论经过多久，缺发送完成事件都不能开始采样。 */
    idd_session_poll(session, now + 10000U);
    CHECK(session->phase == IDD_SESSION_ENTER_TX);
    CHECK(idd_session_tx_complete(session, 1U, now) == -1);
    idd_session_tx_failed(session, 1U, now);
    CHECK(session->phase == IDD_SESSION_ENTER_TX);
    CHECK(idd_session_tx_complete(session, session->result.enter_sequence, now) == 0);
    idd_session_poll(session, now + 4U);
    CHECK(session->phase == IDD_SESSION_SETTLING);
    idd_session_poll(session, now + 5U);
    CHECK(session->phase == IDD_SESSION_CAPTURING);
}

static idd_session_result_t leave(idd_session_t *session, uint32_t now)
{
    idd_session_result_t result;
    CHECK(session->phase == IDD_SESSION_EXIT_READY);
    CHECK(session->points == NULL && session->frozen);
    CHECK(!idd_session_take_result(session, &result));
    frame(session, UINT64_C(0x1020304050607084), 0U);
    CHECK(!idd_session_take_result(session, &result));
    CHECK(idd_session_tx_complete(session, session->result.enter_sequence, now) == -1);
    CHECK(idd_session_tx_complete(session, session->result.exit_sequence, now) == 0);
    CHECK(session->phase == IDD_SESSION_COMPLETE);
    CHECK(idd_session_take_result(session, &result) == 1);
    CHECK(session->phase == IDD_SESSION_IDLE);
    CHECK(!idd_session_take_result(session, &result));
    return result;
}

static void full_window(void)
{
    idd_session_t session = {0};
    uint32_t now = UINT32_MAX - 2U; /* 建立等待和采集均跨越毫秒回绕。 */
    enter(&session, 21U, now, IDD_SESSION_CAPACITY);
    uint32_t start = now + 5U;
    CHECK(idd_session_feed(&session, 100U, start - 1U, start, currents, 7U) == 0);
    for (uint32_t n = 0U; n < IDD_SESSION_CAPACITY; ++n) {
        uint32_t at = n * 1000U / 1024U;
        uint32_t ua[] = {n, UINT32_MAX, n % 7U};
        CHECK(idd_session_feed(&session, UINT32_MAX - 10U + n,
                              start + at, start + at, ua, 7U) == 1);
        CHECK(points[n].current_ua[0] == n && points[n].current_ua[1] == UINT32_MAX);
        CHECK(points[n].offset_ms == at && points[n].valid_mask == 7U);
    }
    CHECK(session.result.stored_samples == 5120U && sizeof(points) == 81920U);
    idd_session_poll(&session, start + 4999U);
    CHECK(session.phase == IDD_SESSION_CAPTURING);
    idd_session_poll(&session, start + 5000U);
    idd_session_result_t result = leave(&session, start + 5003U);
    CHECK(result.flags == 0U && result.collected_ms == 5000U);
    CHECK(result.stored_samples == 5120U && result.boundary_samples == 1U);
    CHECK(result.max_gap_ms == 1U && !result.missing_samples && !result.rejected_samples);
    CHECK(result.current[0].sum_ua == UINT64_C(5119) * 5120U / 2U);
    CHECK(result.current[0].mean_ua == 2559U && result.current[0].minimum_ua == 0U);
    CHECK(result.current[0].maximum_ua == 5119U && result.current[0].valid_count == 5120U);
    CHECK(result.current[1].sum_ua == UINT64_C(5120) * UINT32_MAX);
    CHECK(result.current[1].mean_ua == UINT32_MAX && !result.current[1].failed_count);
    CHECK(result.enter_sequence == UINT64_C(0x1020304050607080));
    CHECK(result.exit_sequence == UINT64_C(0x1020304050607084));
}

static void bad_and_missing_samples(void)
{
    idd_session_t session = {0};
    enter(&session, 36U, 100U, 8U);
    CHECK(idd_session_feed(&session, 1U, 105U, 106U, currents, 7U) == 1);
    CHECK(idd_session_feed(&session, 1U, 106U, 107U, currents, 7U) == 0);
    CHECK(idd_session_feed(&session, 0U, 106U, 107U, currents, 7U) == 0);
    CHECK(idd_session_feed(&session, 2U, 105U, 105U, currents, 7U) == 0);
    CHECK(idd_session_feed(&session, 3U, 107U, 108U, NULL, 0U) == 1);
    CHECK(idd_session_feed(&session, 4U, 108U, 109U, currents, 5U) == 1);
    CHECK(points[1].valid_mask == 0U && points[1].current_ua[0] == 0U);
    idd_session_poll(&session, 5105U);
    idd_session_result_t result = leave(&session, 5108U);
    CHECK(result.flags == (IDD_SESSION_READ_ERROR | IDD_SESSION_SAMPLE_ORDER | IDD_SESSION_SAMPLE_GAP));
    CHECK(result.stored_samples == 3U && result.rejected_samples == 3U && result.missing_samples == 1U);
    CHECK(result.current[0].valid_count == 2U && result.current[0].failed_count == 1U);
    CHECK(result.current[1].valid_count == 1U && result.current[1].failed_count == 2U);
    CHECK(result.current[0].mean_ua == 100U && result.current[1].mean_ua == 200U);
    CHECK(result.max_gap_ms == 4996U);
}

static void overflow_and_empty(void)
{
    idd_session_t session = {0};
    enter(&session, 22U, 0U, 1U);
    CHECK(idd_session_feed(&session, 1U, 5U, 6U, currents, 7U) == 1);
    CHECK(idd_session_feed(&session, 2U, 6U, 7U, currents, 7U) == 0);
    CHECK(points[0].offset_ms == 1U && session.phase == IDD_SESSION_EXIT_READY);
    idd_session_result_t result = leave(&session, 10U);
    CHECK(result.flags == IDD_SESSION_OVERFLOW && result.stored_samples == 1U);
    CHECK(result.collected_ms == 2U);
    enter(&session, 22U, 100U, 8U);
    idd_session_poll(&session, 9000U); /* 无数据也必须结束，不能无限等样本。 */
    result = leave(&session, 9003U);
    CHECK(result.flags == IDD_SESSION_NO_DATA && result.max_gap_ms == 5000U);
    CHECK(!result.stored_samples && result.collected_ms == 5000U);
}

static void cancellation_and_faults(void)
{
    idd_session_t session = {0};
    idd_session_result_t result;
    CHECK(!idd_session_begin(&session, 23U, points, 2U));
    idd_session_cancel(&session, 0U);
    CHECK(idd_session_take_result(&session, &result));
    CHECK((result.flags & IDD_SESSION_CANCELLED) && !result.enter_sequence);

    CHECK(!idd_session_begin(&session, 24U, points, 2U));
    frame(&session, 2U, 1U);
    idd_session_cancel(&session, 0U);
    CHECK(session.phase == IDD_SESSION_ENTER_TX);
    CHECK(!idd_session_tx_complete(&session, 2U, 1U));
    result = leave(&session, 2U);
    CHECK((result.flags & IDD_SESSION_CANCELLED) && !result.stored_samples);

    enter(&session, 25U, 0U, 8U);
    CHECK(idd_session_feed(&session, 1U, 5U, 6U, currents, 7U) == 1);
    idd_session_cancel(&session, 10U);
    result = leave(&session, 13U);
    CHECK(result.flags == IDD_SESSION_CANCELLED && result.collected_ms == 5U);

    enter(&session, 26U, 0U, 8U);
    idd_session_power_off(&session, 6U);
    CHECK(idd_session_take_result(&session, &result));
    CHECK((result.flags & IDD_SESSION_POWER_LOST) && !result.exit_sequence);

    CHECK(!idd_session_begin(&session, 27U, points, 8U));
    frame(&session, 1U, 1U);
    idd_session_tx_failed(&session, 1U, 0U);
    CHECK(idd_session_take_result(&session, &result));
    CHECK(result.flags & IDD_SESSION_TX_ERROR);
    CHECK(idd_session_begin(&session, 27U, points, 8U) == -1);
    idd_session_power_off(&session, 1U);
    CHECK(session.phase == IDD_SESSION_IDLE);

    enter(&session, 28U, 0U, 8U);
    CHECK(idd_session_feed(&session, 1U, 5U, 6U, currents, 7U) == 1);
    idd_session_poll(&session, 5005U);
    frame(&session, 3U, 0U);
    idd_session_tx_failed(&session, 3U, 5006U);
    CHECK(idd_session_take_result(&session, &result));
    CHECK(result.flags == IDD_SESSION_TX_ERROR && result.current[0].valid_count == 1U);
    idd_session_power_off(&session, 6000U);
    CHECK(session.phase == IDD_SESSION_IDLE);
}

static void parameters_and_all_commands(void)
{
    idd_session_t session = {0};
    CHECK(idd_session_begin(NULL, 21U, points, 1U) == -1);
    CHECK(idd_session_begin(&session, 20U, points, 1U) == -1);
    CHECK(idd_session_begin(&session, 37U, points, 1U) == -1);
    CHECK(idd_session_begin(&session, 21U, NULL, 1U) == -1);
    CHECK(idd_session_begin(&session, 21U, points, 0U) == -1);
    CHECK(idd_session_begin(&session, 21U, points, 5121U) == -1);
    CHECK(idd_session_tx_complete(&session, 0U, 0U) == -1);
    for (uint16_t cmd = 21U; cmd <= 36U; ++cmd) {
        enter(&session, cmd, 0U, 2U);
        idd_session_t before = session;
        CHECK(idd_session_begin(&session, cmd, points, 2U) == -1);
        CHECK(!memcmp(&before, &session, sizeof(session)));
        CHECK(idd_session_feed(&session, 1U, 5U, 6U, NULL, 7U) == -1);
        CHECK(idd_session_feed(&session, 1U, 5U, 6U, currents, 8U) == -1);
        CHECK(idd_session_feed(&session, 1U, 5U, 6U, currents, 7U) == 1);
        CHECK(idd_session_feed(&session, 2U, 5004U, 5005U, currents, 7U) == 0);
        idd_session_result_t result = leave(&session, 5006U);
        CHECK(result.command == cmd && result.stored_samples == 1U && result.boundary_samples == 1U);
    }
}

void test_main(void)
{
    full_window();
    bad_and_missing_samples();
    overflow_and_empty();
    cancellation_and_faults();
    parameters_and_all_commands();
    message("PASS: IDD session, 5120 retained points, statistics and cleanup\n");
    finish(0U);
}
