/* 实际C离线故障注入：黄金帧、连续流、单在途事务及双颗粒最终包的独立解码。 */
#include "port.h"
#include "frame.h"
#include "state_machine.h"
#include "crc.h"
#include "rk_link.h"
#include "monitor_task.h"
#include "idd_session.h"
#include <setjmp.h>
#include <string.h>
#include "rk_task_under_test.h"
#if defined(TEST_B1_PROTOCOL_TRACE)
#include "trace_capture.h"
#endif
#define S1(x) #x
#define S(x) S1(x)
#define CHECK(x) do { if (!(x)) fail("FAIL " S(__LINE__) ": " #x "\n"); } while (0)
static uint32_t tick;
static unsigned sent;
static int tx_failure, power_on = PWR_CTRL_STATE_ON;
#if defined(TEST_B1_PROTOCOL_TRACE)
static int uart_idle = 1;
int uart_tx_idle(int id) { (void)id; return uart_idle; }
#include "handoff_test.h"
#endif
/* 仅测试桩使用：记录发送尝试，包含未入队的失败帧；不修改生产任务的接口。 */
static unsigned tx_attempts;
static struct { uint64_t sequence, value; uint32_t tick; uint16_t type, command; } tx_log[16];
static jmp_buf task_exit;
static unsigned task_iterations, delay_calls, health_calls, rx_calls, rx_batch_calls;
static unsigned resume_power_after;
static int rx_mode;
/* 仅用于把CRC正确但TLV非法的完整线帧注入真实RK任务循环。 */
static uint8_t rx_wire[32];
static unsigned rx_wire_length;
static void message(const char *text)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = text;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}
static _Noreturn void finish(uint32_t code)
{
    uint32_t block[] = {0x20026U, code};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) {}
}
static _Noreturn void fail(const char *text) { message(text); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }
uint32_t xTaskGetTickCount(void) { return tick; }
void vTaskDelay(uint32_t delay)
{
    tick += delay;
    if (task_iterations == 0U) return;
    ++delay_calls;
    CHECK(health_calls == delay_calls && rx_batch_calls == 8U);
    rx_batch_calls = 0U;
    if (resume_power_after != 0U && delay_calls == resume_power_after) {
        power_on = PWR_CTRL_STATE_ON;
    }
    if (delay_calls == task_iterations) longjmp(task_exit, 1);
}
void task_watchdog_progress(int task)
{
    CHECK(task == TASK_WATCHDOG_RK);
    ++health_calls;
}
void led_toggle(int led) { (void)led; }
pwr_ctrl_state_t pwr_ctrl_get_state(void) { return (pwr_ctrl_state_t)power_on; }
int uart_poll_rx(int id, uint8_t *buffer, uint16_t length)
{
    CHECK(id == 0);
    if (rx_mode == 0) return 0;
    ++rx_calls;
    /* 配额失效时立即失败，不让真实无限任务卡死整个QEMU回归。 */
    CHECK(++rx_batch_calls <= 8U);
    if (rx_mode == 2) return UART_RX_GAP;
    if (rx_mode == 3) {
        CHECK(rx_wire_length <= length);
        memcpy(buffer, rx_wire, rx_wire_length);
        return (int)rx_wire_length;
    }
    memset(buffer, 0, length);
    return length;
}
int uart_send(int id, uint8_t *buffer, uint16_t length)
{
    CHECK(id == 0 && (length == 22U || length == 16U || length == 30U || length == 34U || length == 27U));
    tlv_frame_t frame;
    CHECK(tlv_unpack(buffer, length, &frame) == (length == 16U ? 0 : length == 22U ? 1 : 2));
    CHECK((length == 22U && frame.units[0].type == TLV_TYPE_CMD) ||
          (length == 16U && frame.count == 0U) ||
          (length == 27U && frame.units[1].type == TLV_TYPE_PARAM_ACTION) ||
          (length == 30U && frame.units[1].type == TLV_TYPE_TIMING_VALUE) ||
          (length == 34U && frame.units[1].type == TLV_TYPE_FREQUENCY_VALUE));
    CHECK(tx_attempts < sizeof(tx_log) / sizeof(tx_log[0]));
    tx_log[tx_attempts].sequence = frame.sequence;
    tx_log[tx_attempts].tick = tick;
    tx_log[tx_attempts].command = frame.count ?
        (uint16_t)(frame.payload[0] | ((uint16_t)frame.payload[1] << 8)) : 0U;
    tx_log[tx_attempts].value = 0U;
    if (frame.count == 2U)
        for (unsigned i = 0U; i < frame.units[1].data_len; ++i)
            tx_log[tx_attempts].value |= (uint64_t)frame.payload[frame.units[1].data_off + i] << (8U * i);
    tx_log[tx_attempts++].type = frame.count ? frame.units[0].type : TLV_TYPE_HEARTBEAT;
    if (tx_failure) return -1;
    ++sent;
    return 0;
}
/* 使用真正的IDD适配/会话，平台桩分别控制入队和最后停止位完成。 */
static monitor_sample_observer_t sample_observer;
static uint32_t tx_ticket, tx_completed_ms;
static int tx_completed;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { storage->locked = 0; return storage; }
int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t wait)
{ (void)wait; CHECK(mutex && !mutex->locked); mutex->locked = 1; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t mutex)
{ CHECK(mutex && mutex->locked); mutex->locked = 0; return pdTRUE; }
void monitor_set_sample_observer(monitor_sample_observer_t observer) { sample_observer = observer; }
int uart_send_tracked(int id, uint8_t *data, uint16_t length, uint32_t *ticket)
{
    if (uart_send(id, data, length) != 0) return -1;
    *ticket = ++tx_ticket;
    tx_completed = 0;
    return 0;
}
int uart_tx_status(int id, uint32_t ticket, uint32_t *completed_ms)
{
    CHECK(id == UART_PORT_RK_PROTOCOL && ticket == tx_ticket);
    if (tx_completed) *completed_ms = tx_completed_ms;
    return tx_completed;
}
static void complete_tx(uint32_t at) { tx_completed_ms = at; tx_completed = 1; }
static void sample_at(uint32_t begin, uint32_t end, uint32_t current, uint8_t valid)
{
    memory_monitor_sample_t sample = {0};
    sample.valid_mask = valid;
    for (unsigned rail = 0; rail < 3U; ++rail) {
        sample.idd[rail].current_ua = current + rail;
        sample.voltage_mv[rail] = 1800 - (int32_t)rail * 600;
    }
    tick = end;
    sample_observer(&sample, MEMORY_MONITOR_VALID_IDD | MEMORY_MONITOR_VALID_VOLTAGE, begin, end);
}
static void pump_samples(uint32_t now)
{ tick = now; sample_observer(NULL, 0U, now, now); }

typedef struct { unsigned count, size; uint8_t data[8][sizeof(rk_response_t)]; } fake_queue_t;
static fake_queue_t requests = {.size = sizeof(rk_request_t)};
static fake_queue_t responses = {.size = sizeof(rk_response_t)};
QueueHandle_t rkRequestQueue = &requests;
QueueHandle_t rkResponseQueue = &responses;
unsigned uxQueueMessagesWaiting(QueueHandle_t handle)
{
    fake_queue_t *queue = handle;
    CHECK(queue != NULL);
    return queue->count;
}
int xQueueSend(QueueHandle_t handle, const void *data, uint32_t wait)
{
    (void)wait;
    fake_queue_t *queue = handle;
    CHECK(queue != NULL);
    if (queue->count == 8U) return 0;
    memcpy(queue->data[queue->count++], data, queue->size);
    return 1;
}
int xQueueReceive(QueueHandle_t handle, void *data, uint32_t wait)
{
    (void)wait;
    fake_queue_t *queue = handle;
    CHECK(queue != NULL);
    if (queue->count == 0U) return 0;
    memcpy(data, queue->data[0], queue->size);
    --queue->count;
    memmove(queue->data[0], queue->data[1], queue->count * sizeof(queue->data[0]));
    return 1;
}
static int cleared(const void *data, unsigned length)
{
    const uint8_t *p = data;
    for (unsigned i = 0; i < length; ++i) if (p[i]) return 0;
    return 1;
}
/* 用户提供的无TLV心跳；两个CRC独立固定，末尾下一帧5A不属于本帧。 */
static void test_empty_heartbeat(void)
{
    static const uint8_t golden[] = {0x5A,0,0,0x51,0xD3,0,0,0,0,0,0,0,5,0x80,0x08,0xA5};
    uint8_t wire[32];
    tlv_frame_t frame;
    CHECK(tlv_pack_heartbeat(5U, wire, sizeof(wire)) == sizeof(golden));
    CHECK(memcmp(wire, golden, sizeof(golden)) == 0);
    CHECK(tlv_unpack(golden, sizeof(golden), &frame) == 0);
    CHECK(frame.count == 0U && frame.payload_used == 0U && frame.sequence == 5U);
}
/* 独立逐位CRC构造畸形TLV，避免打包器的检查挡住故障注入。 */
static unsigned seal(uint8_t *wire, unsigned length)
{
    wire[0] = 0x5A; wire[1] = length >> 8; wire[2] = length;
    uint16_t crc = crc16_modbus_bitwise(wire, 3U);
    wire[3] = crc; wire[4] = crc >> 8;
    crc = crc16_modbus_bitwise(wire + 5, length + 8U);
    wire[length + 13U] = crc; wire[length + 14U] = crc >> 8; wire[length + 15U] = 0xA5;
    return length + 16U;
}
static unsigned feed(tlv_sm_t *stream, const uint8_t *wire, unsigned length)
{
    unsigned frames = 0;
    for (unsigned i = 0; i < length; ++i) {
        if (tlv_sm_feed(stream, wire[i]) == 1) {
            tlv_frame_t frame;
            CHECK(tlv_sm_get_frame(stream, &frame) >= 0);
            ++frames;
        }
    }
    return frames;
}
static tlv_frame_t result_frame(uint64_t sequence, uint16_t command, uint64_t address)
{
    tlv_frame_t frame;
    frame_init(&frame); frame.sequence = sequence;
    uint8_t cmd[] = {(uint8_t)command, (uint8_t)(command >> 8)}, die = 2;
    uint8_t addr[8], elapsed[8] = {0xEF,0xCD,0xAB,0x89,0x67,0x45,0x23,0x01};
    for (unsigned i = 0; i < 8; ++i) addr[i] = address >> (8U * i);
    CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_CMD, cmd, 2U) == 0);
    CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_ADDR, addr, 8U) == 0);
    CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_DIE, &die, 1U) == 0);
    CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_TIME, elapsed, 8U) == 0);
    return frame;
}
/* 独立FAIL标量或畸形单字段进度；打包层只检查字段宽度与取值。 */
static tlv_frame_t scalar_frame(uint64_t sequence, uint16_t type, uint8_t value)
{
    tlv_frame_t frame;
    frame_init(&frame); frame.sequence = sequence;
    CHECK(tlv_pack(&frame, type, &value, 1U) == 0);
    return frame;
}
/* 进度报告由算法号与进度组成；算法号小端，进度为0..100的单字节。 */
static tlv_frame_t progress_frame(uint64_t sequence, uint16_t command, uint8_t percent)
{
    tlv_frame_t frame;
    frame_init(&frame); frame.sequence = sequence;
    uint8_t cmd[] = {(uint8_t)command, (uint8_t)(command >> 8)};
    CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_CMD, cmd, sizeof(cmd)) == 0);
    CHECK(tlv_pack(&frame, TLV_TYPE_PROGRESS, &percent, 1U) == 0);
    return frame;
}
/* 正式事务只接受单个完整最终包；旧四详情构造器仅用于编码兼容/拒绝用例。 */
static tlv_frame_t dual_die_frame(uint8_t mask, uint8_t reverse,
                                  uint64_t first_address, uint64_t second_address,
                                  uint64_t elapsed_ms);
static tlv_frame_t task_result_frame(uint64_t sequence, uint16_t command, uint64_t address)
{
    tlv_frame_t frame = dual_die_frame(address != 0U, 0U, address, 0U, 42000U);
    frame.sequence = sequence;
    frame.payload[0] = (uint8_t)command;
    frame.payload[1] = (uint8_t)(command >> 8);
    return frame;
}
/* 编号和名称按 RK 原始定义逐项核对，不用生产表反向生成预期值。
 * 同一条命令的两种查询应返回同一描述项，并保持FUNC/IDD类别边界。
 */
static void test_command_table(void)
{
    static const char *const names[] = {
        "gtest-stuck-address", "gtest-rand", "gtest-xor", "gtest-sub", "gtest-mul",
        "gtest-div", "gtest-or", "gtest-and", "gtest-seqinc", "gtest-solidbits",
        "gtest-checkerboard", "gtest-blockseq", "gtest-walkbits0", "gtest-walkbits1",
        "gtest-bitspread", "gtest-bitflip", "mats", "marchx", "marchc-", "marchc+",
        "idd_0", "idd_2P", "idd_2PS", "idd_2N", "idd_2NS", "idd_3P", "idd_3PS",
        "idd_3N", "idd_3NS", "idd_4R", "idd_4W", "idd_5", "idd_5AB", "idd_5PB",
        "idd_6", "idd_6ET"
    };
    static const uint16_t unsupported[] = {0U, 37U, UINT16_MAX};
    for (unsigned i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        const rk_command_descriptor_t *command = rk_command_find_by_id((uint16_t)(i + 1U));
        CHECK(command != NULL && command->command_id == i + 1U);
        CHECK(command->name != NULL && strcmp(command->name, names[i]) == 0);
        CHECK(rk_command_find_by_name(names[i]) == command);
        CHECK(command->category == (i < 20U ? RK_COMMAND_CATEGORY_FUNCTIONAL :
                                              RK_COMMAND_CATEGORY_PARAMETRIC));
    }
    for (unsigned i = 0U; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        CHECK(rk_command_find_by_id(unsupported[i]) == NULL);
    }
    CHECK(rk_command_find_by_name(NULL) == NULL);
    CHECK(rk_command_find_by_name("") == NULL);
    CHECK(rk_command_find_by_name("unknown-algorithm") == NULL);
    CHECK(rk_command_find_by_name("IDD_0") == NULL);
    CHECK(rk_command_find_by_name("idd_2p") == NULL);
    CHECK(rk_command_find_by_name("idd_2ps") == NULL);
    CHECK(rk_command_find_by_name("idd_5ab") == NULL);
    CHECK(rk_command_find_by_name("idd_6et") == NULL);
    CHECK(rk_command_find_by_name("idd_0 ") == NULL);
}

/* 已知类型的宽度规则必须同时约束打包和接收；未知扩展仍按显式长度保留。
 * bad_length 都能放入测试缓冲，因此失败应由类型规则引起，而不是缓冲越界。
 */
static void test_type_rules(void)
{
    static const struct {
        uint16_t type;
        uint16_t valid_length;
        uint16_t bad_length;
    } cases[] = {
        {TLV_TYPE_HEARTBEAT,   0U, 1U},
        {TLV_TYPE_CMD,         2U, 1U},
        {TLV_TYPE_RESULT_CMD,  2U, 1U},
        {TLV_TYPE_RESULT_ADDR, 8U, 7U},
        {TLV_TYPE_RESULT_DIE,  1U, 0U},
        {TLV_TYPE_RESULT_TIME, 8U, 9U},
        {TLV_TYPE_PROGRESS,    1U, 0U},
        {TLV_TYPE_RESULT_FAIL, 1U, 2U},
        {TLV_TYPE_PARAM_ACTION, 1U, 0U},
        {TLV_TYPE_REGISTER_ADDRESS, 4U, 3U}
    };
    uint8_t value[9] = {0U}, wire[TLV_FRAME_MAX];
    tlv_frame_t frame, unchanged, decoded;
    for (unsigned i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        frame_init(&frame);
        CHECK(tlv_pack(&frame, cases[i].type, value, cases[i].valid_length) == 0);
        unchanged = frame;
        CHECK(tlv_pack(&frame, cases[i].type, value, cases[i].bad_length) == -1);
        CHECK(memcmp(&frame, &unchanged, sizeof(frame)) == 0);

        /* 绕过打包器写入错误宽度，再独立补 CRC，确认接收端不会接受畸形值。 */
        memset(wire, 0, sizeof(wire));
        wire[13] = (uint8_t)(cases[i].type >> 8);
        wire[14] = (uint8_t)cases[i].type;
        wire[16] = (uint8_t)cases[i].bad_length;
        memset(&decoded, 0xFF, sizeof(decoded));
        unsigned length = seal(wire, 4U + cases[i].bad_length);
        CHECK(tlv_unpack(wire, length, &decoded) == -1 && cleared(&decoded, sizeof(decoded)));
    }

    frame = result_frame(7U, 17U, 0U);
    CHECK(tlv_pack(&frame, 0x7001U, value, 3U) == 0);
    CHECK(tlv_pack(&frame, TLV_TYPE_HEARTBEAT, NULL, 0U) == 0);
    unsigned length = tlv_pack_frame(&frame, 7U, wire, sizeof(wire));
    CHECK(length > 0U && tlv_unpack(wire, length, &decoded) == 6);
    CHECK(decoded.units[4].type == 0x7001U && decoded.units[4].data_len == 3U);
    rk_report_t result;
    CHECK(rk_report_decode(&decoded, &result) == 0 && result.result.command_id == 17U);
    CHECK(result.fields == RK_REPORT_DETAILS);

    /* 完整结果混入命令也不能交付；输出预填非零，检测是否泄漏上一次结果。 */
    CHECK(tlv_pack(&decoded, TLV_TYPE_CMD, value, 2U) == 0);
    memset(&result, 0xFF, sizeof(result));
    CHECK(rk_report_decode(&decoded, &result) == -1 && cleared(&result, sizeof(result)));
}

/* 零TLV不等于放宽坏帧：1..3字节负载、坏CRC、截断和伪造标量仍整帧拒绝。 */
static void test_report_boundaries(void)
{
    uint8_t wire[TLV_FRAME_MAX], golden[16];
    tlv_frame_t frame;
    rk_report_t report;
    tlv_sm_t stream;
    CHECK(tlv_pack_heartbeat(5U, golden, sizeof(golden)) == 16);
    tlv_sm_init(&stream);
    CHECK(feed(&stream, golden, sizeof(golden)) == 1);
    CHECK(feed(&stream, golden, 1U) == 0);
    CHECK(feed(&stream, golden + 1, sizeof(golden) - 1U) == 1);
    for (unsigned i = 0; i < sizeof(golden); ++i) {
        CHECK(tlv_unpack(golden, i, &frame) == -1);
        memcpy(wire, golden, sizeof(golden)); wire[i] ^= 1U;
        CHECK(tlv_unpack(wire, sizeof(golden), &frame) == -1);
    }
    for (unsigned i = 1; i <= 3U; ++i) {
        memset(wire, 0, sizeof(wire)); unsigned length = seal(wire, i);
        CHECK(tlv_unpack(wire, length, &frame) == -1);
        tlv_sm_init(&stream);
        CHECK(feed(&stream, wire, length) == 0);
        CHECK(feed(&stream, golden, sizeof(golden)) == 1);
    }
    frame_init(&frame);
    CHECK(rk_report_decode(&frame, &report) == 0 && report.fields == 0U);
    frame.payload_used = 1U;
    CHECK(tlv_pack_frame(&frame, 1U, wire, sizeof(wire)) == 0);
    CHECK(rk_report_decode(&frame, &report) == -1 && cleared(&report, sizeof(report)));
    static const struct { uint16_t type; uint8_t limit; uint8_t field; } cases[] = {
        {TLV_TYPE_PROGRESS, 100U, RK_REPORT_PROGRESS},
        {TLV_TYPE_RESULT_FAIL, 1U, RK_REPORT_FAIL}
    };
    for (unsigned i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        for (unsigned value = 0U; value <= cases[i].limit; ++value) {
            frame = i == 0U ? progress_frame(9U, 3U, value) :
                              scalar_frame(9U, cases[i].type, value);
            CHECK(rk_report_decode(&frame, &report) == 0 && report.fields == cases[i].field);
            CHECK((i == 0U ? report.progress_percent : report.result.failed) == value);
        }
        uint8_t bad = cases[i].limit + 1U;
        frame_init(&frame);
        CHECK(tlv_pack(&frame, cases[i].type, &bad, 1U) == -1 && frame.count == 0U);
        memset(wire, 0, sizeof(wire));
        wire[13] = cases[i].type >> 8; wire[14] = cases[i].type;
        wire[16] = 1U; wire[17] = bad;
        CHECK(tlv_unpack(wire, seal(wire, 5U), &frame) == -1);
        frame = scalar_frame(9U, cases[i].type, 0U);
        uint8_t zero = 0U;
        CHECK(tlv_pack(&frame, cases[i].type, &zero, 1U) == 0);
        CHECK(rk_report_decode(&frame, &report) == -1 && cleared(&report, sizeof(report)));
    }
    frame = result_frame(9U, 3U, 0U); --frame.count;
    uint8_t failed = 1U;
    CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_FAIL, &failed, 1U) == 0);
    CHECK(rk_report_decode(&frame, &report) == -1 && cleared(&report, sizeof(report)));
}

/* 日志中的MATS进度黄金帧：算法17、进度33%、Seq=0x30，共27字节。 */
static void test_dual_tlv_progress(void)
{
    static const uint8_t golden[] = {
        0x5A,0x00,0x0B,0x10,0x14,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x30,
        0x50,0x01,0x00,0x02,0x11,0x00,0x30,0x16,0x00,0x01,0x21,0x31,0xBC,0xA5
    };
    uint8_t wire[TLV_FRAME_MAX];
    tlv_frame_t frame;
    rk_report_t report;
    CHECK(tlv_unpack(golden, sizeof(golden), &frame) == 2);
    CHECK(rk_report_decode(&frame, &report) == 0);
    CHECK(report.fields == RK_REPORT_PROGRESS && report.result.command_id == 17U);
    CHECK(report.progress_percent == 33U && report.result.elapsed_ms == 0U);
    CHECK(tlv_pack_frame(&frame, 0x30U, wire, sizeof(wire)) == sizeof(golden));
    CHECK(memcmp(wire, golden, sizeof(golden)) == 0);

    /* Type决定含义，交换两个TLV的顺序也必须得到同一算法和进度。 */
    tlv_unit_t first = frame.units[0];
    frame.units[0] = frame.units[1]; frame.units[1] = first;
    CHECK(rk_report_decode(&frame, &report) == 0 && report.result.command_id == 17U);
    CHECK(report.fields == RK_REPORT_PROGRESS && report.progress_percent == 33U);

    frame = scalar_frame(9U, TLV_TYPE_PROGRESS, 33U);
    CHECK(rk_report_decode(&frame, &report) == -1 && cleared(&report, sizeof(report)));
    frame = progress_frame(9U, 17U, 33U); --frame.count;
    CHECK(rk_report_decode(&frame, &report) == -1 && cleared(&report, sizeof(report)));

    const uint16_t duplicate_types[] = {TLV_TYPE_RESULT_CMD, TLV_TYPE_PROGRESS};
    for (unsigned i = 0U; i < 2U; ++i) {
        frame = progress_frame(9U, 17U, 33U);
        uint8_t value[] = {i == 0U ? 17U : 33U, 0U};
        CHECK(tlv_pack(&frame, duplicate_types[i], value, i == 0U ? 2U : 1U) == 0);
        CHECK(rk_report_decode(&frame, &report) == -1 && cleared(&report, sizeof(report)));
    }
    /* 进度不能掩盖残缺详情，也不能与孤立FAIL合成一个假完成结果。 */
    static const struct { uint16_t type, length; } extras[] = {
        {TLV_TYPE_RESULT_ADDR, 8U}, {TLV_TYPE_RESULT_DIE, 1U},
        {TLV_TYPE_RESULT_TIME, 8U}, {TLV_TYPE_RESULT_FAIL, 1U}
    };
    uint8_t zero[8] = {0U};
    for (unsigned i = 0U; i < sizeof(extras) / sizeof(extras[0]); ++i) {
        frame = progress_frame(9U, 17U, 33U);
        CHECK(tlv_pack(&frame, extras[i].type, zero, extras[i].length) == 0);
        CHECK(rk_report_decode(&frame, &report) == -1 && cleared(&report, sizeof(report)));
    }
    /* 沿用未知扩展可忽略、完整结果可带进度的规则，不放宽必需字段校验。 */
    frame = progress_frame(9U, 17U, 33U);
    CHECK(tlv_pack(&frame, 0x7777U, zero, 1U) == 0);
    CHECK(rk_report_decode(&frame, &report) == 0 && report.fields == RK_REPORT_PROGRESS);
    CHECK(report.result.command_id == 17U && report.progress_percent == 33U);
    frame = result_frame(9U, 17U, 0U);
    CHECK(tlv_pack(&frame, TLV_TYPE_PROGRESS, zero, 1U) == 0);
    CHECK(rk_report_decode(&frame, &report) == 0);
    CHECK(report.fields == (RK_REPORT_DETAILS | RK_REPORT_PROGRESS));
}

/* 查表下标损坏时必须先拒绝并复位，不能越界调用；下一帧仍可正常解析。 */
static void test_invalid_states(void)
{
    static const tlv_sm_state_t invalid_states[] = {TLV_SM_COUNT, (tlv_sm_state_t)-1};
    uint8_t wire[TLV_FRAME_MAX];
    unsigned length = tlv_pack_heartbeat(9U, wire, sizeof(wire));
    CHECK(length == 16U);
    for (unsigned i = 0U; i < sizeof(invalid_states) / sizeof(invalid_states[0]); ++i) {
        tlv_sm_t stream;
        tlv_sm_init(&stream);
        stream.state = invalid_states[i];
        stream.pos = 3U;
        stream.ready_len = 16U;
        CHECK(tlv_sm_feed(&stream, 0U) == -1);
        CHECK(stream.state == TLV_SM_IDLE && stream.pos == 0U && stream.ready_len == 0U);
        CHECK(feed(&stream, wire, length) == 1U);
    }
}

static void test_codec(void)
{
    static const uint8_t golden[] = {0x5A,0,6,0xD1,0xD1,0,0,0,0,0,0,0,2,0x50,0,0,2,3,0,0x25,0xA1,0xA5};
    uint8_t wire[TLV_FRAME_MAX + 4U], data[TLV_CMD_DATA_MAX];
    memset(data, 0xA6, sizeof(data)); memset(wire, 0xCC, sizeof(wire));
    CHECK(tlv_pack_cmd(3, NULL, 0, 2, wire, 8) == 0 && wire[0] == 0xCC);
    CHECK(tlv_pack_cmd(3, NULL, 1, 2, wire, sizeof(wire)) == 0);
    CHECK(tlv_pack_cmd(3, data, UINT16_MAX, 2, wire, UINT16_MAX) == 0);
    CHECK(tlv_pack_cmd(3, NULL, 0, 2, wire, sizeof(wire)) == sizeof(golden));
    CHECK(memcmp(wire, golden, sizeof(golden)) == 0 && wire[22] == 0xCC);
    tlv_frame_t frame;
    CHECK(tlv_unpack(wire, sizeof(golden), &frame) == 1 && frame.payload[0] == 3);
    CHECK(frame.sequence == 2 && frame.units[0].data_len == 2);
    for (unsigned i = 0; i < 20U; ++i) {
        memset(&frame, 0xFF, sizeof(frame));
        CHECK(tlv_unpack(wire, i, &frame) == -1 && cleared(&frame, sizeof(frame)));
    }
    for (unsigned i = 0; i < sizeof(golden); ++i) {
        memcpy(wire, golden, sizeof(golden)); wire[i] ^= 1;
        CHECK(tlv_unpack(wire, sizeof(golden), &frame) == -1);
    }
    CHECK(tlv_pack_cmd(0x1234, data, sizeof(data), UINT64_MAX, wire, TLV_FRAME_MAX) == TLV_FRAME_MAX);
    CHECK(tlv_unpack(wire, TLV_FRAME_MAX, &frame) == 1 && frame.sequence == UINT64_MAX);
    CHECK(frame.payload[0] == 0x34 && frame.payload[1] == 0x12 && !memcmp(frame.payload + 2, data, sizeof(data)));
    CHECK(tlv_pack_frame(&frame, 0, wire, 8) == 0);
    frame.units[0].data_off = UINT16_MAX;
    CHECK(tlv_pack_frame(&frame, 0, wire, sizeof(wire)) == 0);
    frame_init(&frame);
    for (unsigned i = 0; i < 16U; ++i) CHECK(tlv_pack(&frame, 0x7001, NULL, 0) == 0);
    CHECK(tlv_pack(&frame, 0x7001, NULL, 0) == -1 && frame.count == 16U);
    CHECK(tlv_pack_frame(&frame, 3, wire, sizeof(wire)) == 80);
    CHECK(tlv_unpack(wire, 80, &frame) == 16);
    /* 在线上直接追加第17个未知TLV，必须原子拒绝。 */
    memset(wire + 13, 0, 68); CHECK(tlv_unpack(wire, seal(wire, 68), &frame) == -1);
    CHECK(cleared(&frame, sizeof(frame)));
    memcpy(wire, golden, sizeof(golden)); wire[16] = 8;
    CHECK(tlv_unpack(wire, seal(wire, 6), &frame) == -1 && cleared(&frame, sizeof(frame)));
    memcpy(wire, golden, sizeof(golden)); memset(wire + 19, 0, 3);
    CHECK(tlv_unpack(wire, seal(wire, 9), &frame) == -1 && cleared(&frame, sizeof(frame)));
    for (unsigned i = 0; i < 4; ++i) {
        tlv_frame_t result = result_frame(7, 17, UINT64_C(0x123456789ABCDEF0));
        tlv_unit_t saved = result.units[0]; result.units[0] = result.units[i]; result.units[i] = saved;
        CHECK(tlv_pack_frame(&result, 7, wire, sizeof(wire)) == 51);
        CHECK(tlv_unpack(wire, 51, &frame) == 4);
        rk_report_t decoded;
        CHECK(rk_report_decode(&frame, &decoded) == 0 && decoded.result.command_id == 17 && decoded.result.die == 2);
        CHECK(decoded.fields == RK_REPORT_DETAILS);
        CHECK(decoded.result.error_address == UINT64_C(0x123456789ABCDEF0) && decoded.result.elapsed_ms == UINT64_C(0x0123456789ABCDEF));
        --frame.count; CHECK(rk_report_decode(&frame, &decoded) == -1 && cleared(&decoded, sizeof(decoded)));
        frame = result; frame.units[0] = frame.units[1];
        memset(&decoded, 0xFF, sizeof(decoded));
        CHECK(rk_report_decode(&frame, &decoded) == -1 && cleared(&decoded, sizeof(decoded)));
    }
    tlv_sm_t stream; tlv_sm_init(&stream);
    CHECK(tlv_sm_get_frame(&stream, &frame) == -1);
    CHECK(feed(&stream, golden, sizeof(golden)) == 1);
    CHECK(feed(&stream, golden, sizeof(golden)) == 1);
    CHECK(feed(&stream, (uint8_t[]){0x5A,0x99}, 2) == 0);
    CHECK(feed(&stream, golden, sizeof(golden)) == 1);
    CHECK(feed(&stream, golden, 8) == 0); tick += RK_BYTE_TIMEOUT_MS;
    CHECK(feed(&stream, golden, sizeof(golden)) == 1);
    memcpy(wire, golden, sizeof(golden)); wire[19] ^= 1;
    CHECK(feed(&stream, wire, sizeof(golden)) == 0);
    CHECK(feed(&stream, golden, sizeof(golden)) == 1);
    CHECK(feed(&stream, golden, sizeof(golden) - 1) == 0);
    CHECK(feed(&stream, golden, sizeof(golden)) == 1);
    tick = UINT32_MAX - 20; CHECK(feed(&stream, golden, 8) == 0);
    tick += RK_BYTE_TIMEOUT_MS; CHECK(feed(&stream, golden, sizeof(golden)) == 1);
}

/* 动作接口仅组两TLV帧；独立黄金CRC约束它不能把action追加到5000内部。 */
static void test_idd_action_codec(void)
{
    static const uint8_t golden[2][27] = {
        {0x5A,0x00,0x0B,0x10,0x14,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
         0x50,0x00,0x00,0x02,0x15,0x00,0x10,0x1A,0x00,0x01,0x00,0x60,0x9C,0xA5},
        {0x5A,0x00,0x0B,0x10,0x14,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
         0x50,0x00,0x00,0x02,0x15,0x00,0x10,0x1A,0x00,0x01,0x01,0xA1,0x5C,0xA5}
    };
    uint8_t wire[28], untouched[28];
    tlv_frame_t frame;
    for (uint8_t action = 0U; action < 2U; ++action) {
        memset(wire, 0xCC, sizeof(wire));
        CHECK(tlv_pack_idd_action(21U, action, UINT64_C(0x0102030405060708),
                                  wire, 27U) == TLV_IDD_ACTION_FRAME_SIZE);
        CHECK(memcmp(wire, golden[action], 27U) == 0 && wire[27] == 0xCC);
        CHECK(tlv_unpack(wire, 27U, &frame) == 2 && frame.payload_used == 3U);
        CHECK(frame.sequence == UINT64_C(0x0102030405060708));
        CHECK(frame.units[0].type == TLV_TYPE_CMD && frame.units[0].data_len == 2U);
        CHECK(frame.units[1].type == TLV_TYPE_PARAM_ACTION && frame.units[1].data_len == 1U);
        CHECK(frame.payload[0] == 21U && frame.payload[1] == 0U && frame.payload[2] == action);
        tlv_sm_t stream; tlv_sm_init(&stream);
        CHECK(feed(&stream, wire, 27U) == 1U);
        wire[24] ^= 1U;
        CHECK(tlv_unpack(wire, 27U, &frame) == -1);
    }
    CHECK(tlv_pack_idd_action(36U, 0U, UINT64_MAX, wire, sizeof(wire)) == 27);
    CHECK(tlv_unpack(wire, 27U, &frame) == 2 && frame.payload[0] == 36U);
    /* 非法值、非IDD编号和短缓冲都必须在写输出之前失败。 */
    memset(wire, 0xCC, sizeof(wire)); memcpy(untouched, wire, sizeof(wire));
    CHECK(tlv_pack_idd_action(21U, 2U, 0U, wire, sizeof(wire)) == 0);
    CHECK(tlv_pack_idd_action(21U, UINT8_MAX, 0U, wire, sizeof(wire)) == 0);
    CHECK(tlv_pack_idd_action(20U, 1U, 0U, wire, sizeof(wire)) == 0);
    CHECK(tlv_pack_idd_action(37U, 1U, 0U, wire, sizeof(wire)) == 0);
    CHECK(tlv_pack_idd_action(21U, 1U, 0U, wire, 26U) == 0);
    CHECK(tlv_pack_idd_action(21U, 1U, 0U, NULL, 27U) == 0);
    CHECK(memcmp(wire, untouched, sizeof(wire)) == 0);
    /* 接收端也拒绝CRC正确但bool值为2的动作帧。 */
    memcpy(wire, golden[0], 27U); wire[23] = 2U;
    unsigned length = seal(wire, 11U);
    CHECK(length == 27U && tlv_unpack(wire, length, &frame) == -1);
}

/* 7018仅携带无符号32位地址；用最高位为1的黄金值约束小端编码，不执行寄存器读写。 */
static void test_register_address_codec(void)
{
    static const uint8_t golden[] = {
        0x5A,0x00,0x08,0x50,0x15,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x70,0x18,0x00,0x04,0x98,0xBA,0xDC,0xFE,0x8D,0x58,0xA5
    };
    const uint32_t address = UINT32_C(0xFEDCBA98);
    uint8_t value[5] = {(uint8_t)address, (uint8_t)(address >> 8),
                        (uint8_t)(address >> 16), (uint8_t)(address >> 24), 0U};
    uint8_t wire[28];
    tlv_frame_t frame, decoded;
    frame_init(&frame);
    CHECK(tlv_pack(&frame, TLV_TYPE_REGISTER_ADDRESS, value, 4U) == 0);
    CHECK(tlv_pack_frame(&frame, UINT64_C(0x0102030405060708), wire, sizeof(wire)) == 24);
    CHECK(memcmp(wire, golden, sizeof(golden)) == 0);
    CHECK(tlv_unpack(wire, 24U, &decoded) == 1);
    CHECK(decoded.units[0].type == TLV_TYPE_REGISTER_ADDRESS && decoded.units[0].data_len == 4U);
    const uint8_t *decoded_value = tlv_unit_get_data(&decoded, &decoded.units[0]);
    uint32_t decoded_address = (uint32_t)decoded_value[0] | ((uint32_t)decoded_value[1] << 8) |
                              ((uint32_t)decoded_value[2] << 16) | ((uint32_t)decoded_value[3] << 24);
    CHECK(decoded_address == address);
    tlv_frame_t unchanged = frame;
    CHECK(tlv_pack(&frame, TLV_TYPE_REGISTER_ADDRESS, value, 5U) == -1);
    CHECK(memcmp(&frame, &unchanged, sizeof(frame)) == 0);
    /* 三字节宽度由通用规则用例覆盖；此处补五字节且CRC正确的接收拒绝。 */
    memcpy(wire, golden, sizeof(golden)); wire[16] = 5U; wire[21] = 0U;
    unsigned length = seal(wire, 9U);
    CHECK(length == 25U && tlv_unpack(wire, length, &decoded) == -1);
    CHECK(cleared(&decoded, sizeof(decoded)));
}

/* 覆盖56字节MATS结果的全部切分点、主动检查/继续收字节和tick回绕。
 * V2与B1门限均固定100ms，测试须直接约束两工程，防止独立工程漏同步。
 */
static void test_byte_timeout(void)
{
    static const uint8_t golden[] = {
        0x5A,0x00,0x28,0x51,0xCD,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x30,
        0x50,0x01,0x00,0x02,0x11,0x00,0x10,0x17,0x00,0x01,0x00,
        0x90,0x01,0x00,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x30,0x01,0x00,0x01,0x00,0x90,0x02,0x00,0x08,
        0x0D,0x16,0x00,0x00,0x00,0x00,0x00,0x00,0x8F,0x68,0xA5
    };
    const uint32_t limit = RK_BYTE_TIMEOUT_MS;
    CHECK(limit == 100U);
    const uint32_t delays[] = {0U, limit / 2U, limit - 1U, limit, limit + 1U};
    for (unsigned wrap = 0U; wrap < 2U; ++wrap) {
        for (unsigned poll = 0U; poll < 2U; ++poll) {
            for (unsigned d = 0U; d < sizeof(delays) / sizeof(delays[0]); ++d) {
                for (unsigned split = 1U; split < sizeof(golden); ++split) {
                    tlv_sm_t stream;
                    tlv_sm_init(&stream);
                    tick = wrap ? UINT32_MAX - 20U : 0U;
                    CHECK(feed(&stream, golden, split) == 0U);
                    tick += delays[d];
                    unsigned expired = delays[d] >= limit;
                    if (poll) CHECK(tlv_sm_check_timeout(&stream, limit) == (int)expired);
                    CHECK(feed(&stream, golden + split, sizeof(golden) - split) == !expired);
                    /* 不足门限即正常拼帧；超时清理后也必须能接收下一完整帧。 */
                    CHECK(feed(&stream, golden, sizeof(golden)) == 1U);
                }
            }
        }
    }
    tick = 0U;
}
static void test_transaction(void)
{
    rk_request_t request = {3, 101, 5000, 0U};
    rk_response_t response;
    CHECK(rk_link_request(NULL) == -1);
    request.command_id = 37; CHECK(rk_link_request(&request) == -1); request.command_id = 3;
    request.timeout_ms = UINT32_MAX; CHECK(rk_link_request(&request) == -1); request.timeout_ms = 5000;
    CHECK(rk_link_request(&request) == 0); process_request(); CHECK(sent == 1 && s_pending.waiting);
    tick += 600; process_request(); CHECK(sent == 1 && s_pending.waiting);
    tlv_frame_t result = task_result_frame(s_pending.sequence + 1U, 3, 0);
    accept_frame(&result); CHECK(s_pending.waiting);
    result = task_result_frame(s_pending.sequence, 4, 0); accept_frame(&result); CHECK(s_pending.waiting);
    result = task_result_frame(s_pending.sequence, 3, 0); --result.count;
    accept_frame(&result); CHECK(s_pending.waiting);
    result = task_result_frame(s_pending.sequence, 3, 0);
    responses.count = 8; accept_frame(&result); process_request();
    CHECK(s_pending.delivery && !s_pending.waiting && sent == 1);
    responses.count = 0; process_request(); CHECK(rk_link_receive(&response) == 1);
    CHECK(response.status == RK_RESPONSE_COMPLETE && response.cookie == 101 && response.result.failure_count == 0U);
    accept_frame(&result); process_request(); CHECK(rk_link_receive(&response) == 0);
    tx_failure = 1; CHECK(rk_link_request(&request) == 0); process_request(); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_TX_FAILED && sent == 1);
    tx_failure = 0; request.timeout_ms = 1000;
    CHECK(rk_link_request(&request) == 0); process_request(); CHECK(sent == 2);
    tick += 1000; process_request(); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_TIMEOUT);
    CHECK(rk_link_request(&request) == 0); tick += 10000; process_request(); CHECK(sent == 2);
    result = task_result_frame(s_pending.sequence, 3, 1); accept_frame(&result); process_request();
    CHECK(sent == 3 && rk_link_receive(&response) == 1);
    CHECK(response.status == RK_RESPONSE_RECOVERED && response.cookie == 101U);
    CHECK(cleared(&response.result, sizeof(response.result)) && rk_link_receive(&response) == 0);
    result = task_result_frame(s_pending.sequence, 3, 1); accept_frame(&result); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.result.failures[0].error_address == 1);
#if defined(B1_RK_UART)
    CHECK(rk_link_request(&request) == 0); process_request();
    power_on = 0; CHECK(rk_link_request(&request) == -1);
    process_power_off(); CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_POWER_OFF);
#endif
}

/* 每组故障注入清理桩与事务；RK序号仍由真实分配器持续递增，不在桩里重造。 */
static void reset_task_test(void)
{
    rk_idd_init();
    rk_idd_power_off();
    s_timing_gap = 0U;
    uint32_t frequency_token;
    (void)rk_link_frequency_get(&frequency_token);
    rk_link_frequency_end(frequency_token);
    memset(&s_pending, 0, sizeof(s_pending));
    s_rx_diagnostics = (rk_rx_diagnostics_t){0};
    requests.count = responses.count = 0U;
    tick = sent = tx_attempts = 0U;
    tx_failure = 0;
    power_on = PWR_CTRL_STATE_ON;
    task_iterations = delay_calls = health_calls = rx_calls = rx_batch_calls = 0U;
    resume_power_after = 0U;
    rx_mode = 0;
    rx_wire_length = 0U;
}

/* 全16项进入/退出与采样窗口；RK进度/最终包不能提前释放事务。 */
static void test_idd_transactions(void)
{
    for (uint16_t command = 21U; command <= 36U; ++command) {
        reset_task_test();
        rk_request_t request = {command, 1000U + command, 0U, 0U};
        CHECK(rk_link_request(&request) == 0);
        process_request();
        CHECK(sent == 1U && s_pending.waiting && tx_log[0].value == 1U);
        CHECK(tx_log[0].command == command);
        tlv_frame_t progress = progress_frame(s_pending.sequence, command, 50U);
        accept_frame(&progress);
        tlv_frame_t result = task_result_frame(s_pending.sequence, command, 0U);
        accept_frame(&result); process_request();
        rk_response_t response;
        CHECK(s_pending.waiting && rk_link_receive(&response) == 0);
        /* 入队后尚未TC的样本不计；TC=100，采集起点必须105，不能取任务醒来时刻。 */
        sample_at(90U, 91U, 999999U, MEMORY_MONITOR_VALID_ALL);
        complete_tx(100U);
        sample_at(104U, 105U, 999999U, MEMORY_MONITOR_VALID_ALL);
        sample_at(105U, 105U, 1000U, MEMORY_MONITOR_VALID_ALL);
        sample_at(105U, 105U, 3000U, MEMORY_MONITOR_VALID_ALL); /* 同毫秒新读数也保存。 */
        for (uint32_t now = 1105U; now < 5105U; now += 1000U) pump_samples(now);
        sample_at(5103U, 5104U, 5000U, MEMORY_MONITOR_VALID_ALL);
        pump_samples(5105U); process_request();
        CHECK(sent == 2U && tx_log[1].command == command && tx_log[1].value == 0U);
        CHECK(s_pending.waiting && rk_link_receive(&response) == 0);
        /* 观察者等待锁前记录的旧结束时刻不能令退出TX超时减法回绕。 */
        sample_observer(NULL, 0U, 5100U, 5100U);
        process_request(); CHECK(s_pending.waiting && rk_link_receive(&response) == 0);
        sample_at(5110U, 5111U, 999999U, MEMORY_MONITOR_VALID_ALL);
        complete_tx(5112U); tick = 5113U;
        process_request(); process_request();
        CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_COMPLETE);
        CHECK(response.cookie == request.cookie && response.result.command_id == command);
        CHECK(response.result.failed == 0U && response.result.failure_count == 0U);
        CHECK(response.measurement.state == IDD_STATS_COMPLETE);
        CHECK(response.measurement.current[0].valid_count == 3U);
        CHECK(response.measurement.current[0].sum == 9000 && response.measurement.current[0].mean == 3000);
        CHECK(response.measurement.current[0].minimum == 1000 && response.measurement.current[0].maximum == 5000);
        CHECK(response.measurement.voltage[0].valid_count == 2U);
        accept_frame(&result); process_request();
        CHECK(rk_link_receive(&response) == 0 && sent == 2U && !s_pending.waiting);
    }
    reset_task_test();
}

/* 故障不能形成默认PASS；无RK ACK时仍须保证退出发送完成和单在途背压。 */
static void test_idd_failures(void)
{
    rk_request_t request = {21U, 500U, 0U, 0U};
    rk_response_t response;
    reset_task_test(); tx_failure = 1;
    CHECK(rk_link_request(&request) == 0); process_request(); process_request();
    CHECK(!s_pending.waiting && rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_TX_FAILED);
    /* 进入没有TC，不能开始采样，也不能因RK最终包或下一请求解除占用。 */
    reset_task_test(); CHECK(rk_link_request(&request) == 0); process_request();
    tick = 1000U; process_request(); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_TIMEOUT && s_pending.waiting);
    tlv_frame_t stale = task_result_frame(s_pending.sequence, request.command_id, 0U);
    accept_frame(&stale); CHECK(s_pending.waiting);
    CHECK(rk_link_request(&request) == 0); process_request(); CHECK(requests.count == 1U && sent == 1U);
#if defined(B1_RK_UART)
    power_on = PWR_CTRL_STATE_OFF; process_power_off();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_RECOVERED);
#endif
    /* 依次注入空窗口、无效电流、容量溢出、退出入队失败、退出TC丢失、采样停滞。 */
    for (unsigned fault = 0U; fault < 6U; ++fault) {
        reset_task_test(); CHECK(rk_link_request(&request) == 0); process_request();
        complete_tx(0U); pump_samples(5U);
        if (fault == 1U) sample_at(5U, 5U, 1000U, MEMORY_MONITOR_VALID_VOLTAGE);
        if (fault >= 2U) sample_at(5U, 5U, 1000U, MEMORY_MONITOR_VALID_ALL);
        if (fault == 2U) {
            for (unsigned n = 0U; n < IDD_SESSION_CAPACITY; ++n)
                sample_at(6U, 6U, n, MEMORY_MONITOR_VALID_ALL);
        } else if (fault == 5U) {
            tick = 1006U; /* 没有生产者轮末事件，必须走异常退出。 */
        } else {
            for (uint32_t now = 1005U; now <= 5005U; now += 1000U) pump_samples(now);
        }
        tx_failure = fault == 3U;
        process_request();
        if (fault == 4U) { tick += 1000U; process_request(); }
        else if (fault != 3U) { complete_tx(tick + 1U); ++tick; process_request(); }
        process_request();
        CHECK(rk_link_receive(&response) == 1);
        CHECK(response.status == ((fault == 3U || fault == 4U) ? RK_RESPONSE_TIMEOUT : RK_RESPONSE_MEASUREMENT_FAILED));
        CHECK(s_pending.waiting == (fault == 3U || fault == 4U));
        CHECK(rk_link_receive(&response) == 0);
    }
    /* 显式timeoutMs只报一次TIMEOUT，退出TC后再RECOVERED；绝不补报0109。 */
    reset_task_test(); request.timeout_ms = 100U;
    CHECK(rk_link_request(&request) == 0); process_request(); complete_tx(0U);
    sample_at(5U, 6U, 1000U, MEMORY_MONITOR_VALID_ALL);
    tick = 100U; process_request(); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_TIMEOUT && s_pending.waiting);
    complete_tx(101U); tick = 101U; process_request(); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_RECOVERED && !s_pending.waiting);
    /* 窗口跨毫秒回绕，结果队列满时保留冻结统计；下一请求不能覆盖。 */
    reset_task_test(); request.timeout_ms = 0U; tick = UINT32_MAX - 10U;
    CHECK(rk_link_request(&request) == 0); process_request(); complete_tx(tick);
    uint32_t start = tick + 5U;
    sample_at(start, start, 1111U, MEMORY_MONITOR_VALID_ALL);
    for (unsigned n = 1U; n <= 5U; ++n) pump_samples(start + n * 1000U);
    process_request(); complete_tx(tick + 1U); ++tick;
    responses.count = 8U; CHECK(rk_link_request(&request) == 0);
    process_request(); process_request();
    CHECK(s_pending.delivery && requests.count == 1U);
    CHECK(s_pending.response.measurement.current[0].sum == 1111);
    responses.count = 0U; process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_COMPLETE);
    CHECK(response.measurement.current[0].valid_count == 1U && response.measurement.current[0].sum == 1111);
    reset_task_test();
    message("PASS: IDD sampling/TX faults, explicit timeout, power recovery, wrap and queue backpressure\n");
}

/* 拒绝记录只增加可观测性；合法帧、窗口及最终结果交付必须维持原合同。 */
static void test_rx_diagnostics(void)
{
    reset_task_test();
    rk_rx_diagnostics_t diag;
    CHECK(rk_link_get_rx_diagnostics(NULL) == -1);
    CHECK(rk_link_get_rx_diagnostics(&diag) == 0 && diag.rejected_total == 0U);
    CHECK(diag.last_reason == RK_RX_REJECT_NONE);
    tlv_frame_t frame;
    frame_init(&frame); frame.sequence = UINT64_MAX;
    accept_frame(NULL); accept_frame(&frame);
    CHECK(tlv_pack(&frame, TLV_TYPE_HEARTBEAT, NULL, 0U) == 0);
    accept_frame(&frame);
    CHECK(rk_link_get_rx_diagnostics(&diag) == 0 && diag.rejected_total == 0U);

    frame = task_result_frame(42U, 17U, 0U);
    accept_frame(&frame);
    CHECK(rk_link_get_rx_diagnostics(&diag) == 0 && diag.rejected_total == 1U);
    CHECK(diag.last_reason == RK_RX_REJECT_NO_PENDING && !diag.waiting);
    CHECK(diag.frame_valid && diag.received_sequence == 42U && diag.tlv_count == 3U);
    CHECK(diag.command_sequence == 0U && diag.last_tx_sequence == 0U && diag.expected_command == 0U);

    rk_request_t request = {17U, 730U, 0U, 0U};
    CHECK(rk_link_request(&request) == 0); process_request();
    uint64_t sequence = s_pending.sequence;
    rk_pending_t pending = s_pending;
    frame_init(&frame); frame.sequence = sequence - 1U;
    accept_frame(&frame); /* 有在途时，窗口外普通心跳同样不算业务拒绝。 */
    CHECK(rk_link_get_rx_diagnostics(&diag) == 0 && diag.rejected_total == 1U);

    tick = 123U;
    frame = task_result_frame(sequence - 1U, 17U, 0U);
    accept_frame(&frame);
    frame.sequence = sequence + 1U;
    accept_frame(&frame);
    CHECK(rk_link_get_rx_diagnostics(&diag) == 0 && diag.rejected_total == 3U);
    CHECK(diag.counts[RK_RX_REJECT_SEQUENCE] == 2U && diag.last_reason == RK_RX_REJECT_SEQUENCE);
    CHECK(diag.last_tick == 123U && diag.received_sequence == sequence + 1U);
    CHECK(diag.waiting && diag.command_sequence == sequence && diag.last_tx_sequence == sequence);
    CHECK(diag.expected_command == 17U && diag.received_command == 0U);
    CHECK(memcmp(&s_pending, &pending, sizeof(pending)) == 0);

    frame = task_result_frame(sequence, 18U, 0U); accept_frame(&frame);
    frame = progress_frame(sequence, 18U, 50U); accept_frame(&frame);
    CHECK(rk_link_get_rx_diagnostics(&diag) == 0 && diag.rejected_total == 5U);
    CHECK(diag.counts[RK_RX_REJECT_ALGORITHM] == 2U && diag.received_command == 18U);
    CHECK(diag.last_reason == RK_RX_REJECT_ALGORITHM && !s_pending.progress_valid);

    frame = scalar_frame(sequence, TLV_TYPE_PROGRESS, 50U); accept_frame(&frame);
    CHECK(rk_link_get_rx_diagnostics(&diag) == 0 && diag.last_reason == RK_RX_REJECT_PROGRESS);
    CHECK(diag.counts[RK_RX_REJECT_PROGRESS] == 1U && diag.tlv_count == 1U);
    frame = task_result_frame(sequence, 17U, 0U); --frame.count; accept_frame(&frame);
    CHECK(rk_link_get_rx_diagnostics(&diag) == 0 && diag.last_reason == RK_RX_REJECT_RESULT);
    CHECK(diag.counts[RK_RX_REJECT_RESULT] == 1U && diag.rejected_total == 7U);
    CHECK(memcmp(&s_pending, &pending, sizeof(pending)) == 0);

    /* 合法进度与最终包不抹掉最近一次拒绝，也不能因诊断改成重复交付。 */
    frame = progress_frame(sequence, 17U, 50U); accept_frame(&frame);
    CHECK(s_pending.progress_valid && s_pending.progress_percent == 50U);
    frame = task_result_frame(sequence, 17U, 0U); accept_frame(&frame); process_request();
    rk_response_t response;
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_COMPLETE);
    CHECK(rk_link_get_rx_diagnostics(&diag) == 0 && diag.rejected_total == 7U);
    CHECK(diag.last_reason == RK_RX_REJECT_RESULT);
    accept_frame(&frame); process_request();
    CHECK(rk_link_receive(&response) == 0);
    CHECK(rk_link_get_rx_diagnostics(&diag) == 0 && diag.rejected_total == 8U);
    CHECK(diag.last_reason == RK_RX_REJECT_NO_PENDING && diag.counts[RK_RX_REJECT_NO_PENDING] == 2U);

    /* 仅测试桩预置饱和边界，生产接口不提供清零或改写能力。 */
    CHECK(rk_link_request(&request) == 0); process_request();
    s_rx_diagnostics.rejected_total = UINT32_MAX;
    s_rx_diagnostics.counts[RK_RX_REJECT_SEQUENCE] = UINT32_MAX;
    frame = task_result_frame(s_pending.sequence - 1U, 17U, 0U); accept_frame(&frame);
    CHECK(rk_link_get_rx_diagnostics(&diag) == 0 && diag.rejected_total == UINT32_MAX);
    CHECK(diag.counts[RK_RX_REJECT_SEQUENCE] == UINT32_MAX && diag.last_reason == RK_RX_REJECT_SEQUENCE);
    CHECK(diag.counts[RK_RX_REJECT_NONE] == 0U && s_pending.waiting);
    reset_task_test();
}

/* 进度必须匹配本次算法及Seq窗口，不改变最终结果缓存或测试生命周期。 */
static void test_progress_matching(void)
{
    reset_task_test();
    rk_request_t request = {17U, 706U, 0U, 0U};
    rk_response_t response;
    CHECK(rk_link_request(&request) == 0); process_request();
    uint64_t sequence = s_pending.sequence;
    tlv_frame_t frame = progress_frame(sequence, 18U, 33U);
    accept_frame(&frame); CHECK(!s_pending.progress_valid);
    frame = scalar_frame(sequence, TLV_TYPE_PROGRESS, 33U);
    accept_frame(&frame); CHECK(!s_pending.progress_valid);
    frame = progress_frame(sequence - 1U, 17U, 33U);
    accept_frame(&frame); CHECK(!s_pending.progress_valid);
    frame.sequence = sequence + 1U;
    accept_frame(&frame); CHECK(!s_pending.progress_valid);

    frame.sequence = sequence;
    accept_frame(&frame);
    CHECK(s_pending.progress_valid && s_pending.progress_percent == 33U);
    frame = progress_frame(sequence, 18U, 99U);
    accept_frame(&frame); CHECK(s_pending.progress_percent == 33U);
    frame = progress_frame(sequence, 17U, 100U);
    accept_frame(&frame); process_request();
    CHECK(s_pending.progress_percent == 100U && s_pending.progress_sequence == sequence);
    CHECK(s_pending.waiting);
    CHECK(rk_link_receive(&response) == 0);

    frame = task_result_frame(sequence, 17U, 0U);
    accept_frame(&frame); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_COMPLETE);
    CHECK(response.result.command_id == 17U && response.result.failed == 0U);
    frame = progress_frame(sequence, 17U, 20U);
    accept_frame(&frame); process_request();
    CHECK(s_pending.progress_percent == 100U && rk_link_receive(&response) == 0);
    reset_task_test();
}

/* 3/5/7 TLV一次交付；旧两包、错误算法、重复结果及非法配对不得制造完成。 */
static void test_atomic_result(void)
{
    for (uint8_t mask = 0U; mask < 4U; ++mask) {
        for (uint8_t reverse = 0U; reverse < 2U; ++reverse) {
            reset_task_test();
            rk_request_t request = {17U, 701U, 0U, 0U};
            rk_response_t response;
            CHECK(rk_link_request(&request) == 0); process_request();
            uint64_t sequence = s_pending.sequence;
            tlv_frame_t old = result_frame(sequence, 17U, 0U);
            tlv_frame_t flag = scalar_frame(sequence, TLV_TYPE_RESULT_FAIL, mask != 0U);
            accept_frame(&old); accept_frame(&flag);
            CHECK(s_pending.waiting && rk_link_receive(&response) == 0);
            tlv_frame_t result = dual_die_frame(mask, reverse, 1U, UINT64_MAX, UINT64_MAX);
            result.sequence = sequence;
            tlv_frame_t wrong = result; wrong.payload[0] = 18U;
            accept_frame(&wrong); CHECK(s_pending.waiting);
            wrong = result; --wrong.count;
            accept_frame(&wrong); CHECK(s_pending.waiting);
            tick += 20U * 60U * 1000U; process_request();
            CHECK(s_pending.waiting && sent == 1U);
            accept_frame(&result); accept_frame(&result); process_request();
            CHECK(!s_pending.waiting && rk_link_receive(&response) == 1);
            CHECK(response.status == RK_RESPONSE_COMPLETE && response.result.failed == (mask != 0U));
            CHECK(response.result.failure_count == ((mask & 1U) != 0U) + ((mask & 2U) != 0U));
            CHECK(response.result.command_id == 17U && response.result.elapsed_ms == UINT64_MAX);
            for (uint8_t i = 0U; i < response.result.failure_count; ++i) {
                const rk_failure_detail_t *failure = &response.result.failures[i];
                CHECK(failure->error_address == (failure->die == 0U ? 1U : UINT64_MAX));
            }
            CHECK(response.sequence == sequence && response.cookie == 701U);
            CHECK(rk_link_receive(&response) == 0);
            CHECK(rk_link_request(&request) == 0); process_request();
            CHECK(!s_pending.progress_valid);
            accept_frame(&result); CHECK(s_pending.waiting);
        }
    }
    reset_task_test();
}

/* RK统一携带最近收到的MCU流水号，两包可以不同，不能强求等于测试命令Seq。 */
static void test_latest_received_sequence(void)
{
    for (unsigned order = 0U; order < 2U; ++order) {
        reset_task_test();
        rk_request_t request = {3U, 704U, 0U, 0U};
        rk_response_t response;
        CHECK(rk_link_request(&request) == 0); process_request();
        uint64_t command_seq = s_pending.sequence;
        TickType_t last = 0U;
        tick = 1000U; process_heartbeat(&last);
        uint64_t first_seq = tx_log[1].sequence;
        tick = 2000U; process_heartbeat(&last);
        uint64_t second_seq = tx_log[2].sequence;
        CHECK(first_seq != command_seq && second_seq != first_seq);
        tlv_frame_t stray = task_result_frame(command_seq - 1U, 3U, 0U);
        accept_frame(&stray); CHECK(s_pending.waiting);
        stray.sequence = second_seq + 1U;
        accept_frame(&stray); CHECK(s_pending.waiting);
        /* 发送失败的心跳不能推进接收窗口，也不能被RK当成已收到的新Seq。 */
        tx_failure = 1; tick = 3000U; process_heartbeat(&last); tx_failure = 0;
        CHECK(s_pending.last_tx_sequence == second_seq);
        stray.sequence = tx_log[3].sequence;
        accept_frame(&stray); CHECK(s_pending.waiting);
        /* RK定时上报可落后两个Seq；同一Seq的多条进度帧仍逐帧检查内容。 */
        tlv_frame_t progress = progress_frame(command_seq, 3U, 50U);
        accept_frame(&progress);
        CHECK(s_pending.progress_valid && s_pending.progress_percent == 50U);
        CHECK(s_pending.progress_sequence == command_seq && s_pending.waiting);
        for (uint8_t percent = 70U; percent <= 72U; ++percent) {
            progress = progress_frame(first_seq, 3U, percent);
            accept_frame(&progress);
            CHECK(s_pending.progress_percent == percent);
            CHECK(s_pending.waiting);
        }
        CHECK(s_pending.progress_valid && s_pending.progress_percent == 72U);
        CHECK(s_pending.progress_sequence == first_seq && s_pending.waiting);
        tlv_frame_t result = task_result_frame(order ? second_seq : first_seq, 3U, 1U);
        accept_frame(&result); process_request();
        CHECK(!s_pending.waiting && rk_link_receive(&response) == 1);
        CHECK(response.status == RK_RESPONSE_COMPLETE && response.result.failed == 1U);
        CHECK(response.cookie == 704U && response.sequence == command_seq);
        CHECK(rk_link_receive(&response) == 0);
    }
    /* 只注入事务窗口边界，验证uint64回绕；不修改正式发送序号分配器。 */
    reset_task_test();
    rk_request_t request = {3U, 705U, 0U, 0U};
    rk_response_t response;
    CHECK(rk_link_request(&request) == 0); process_request();
    s_pending.sequence = UINT64_MAX - 1U;
    s_pending.last_tx_sequence = 1U;
    tlv_frame_t progress = progress_frame(0U, 3U, 50U);
    accept_frame(&progress); CHECK(s_pending.progress_valid && s_pending.progress_percent == 50U);
    tlv_frame_t flag = task_result_frame(UINT64_MAX - 2U, 3U, 1U);
    accept_frame(&flag); CHECK(s_pending.waiting);
    flag.sequence = 2U; accept_frame(&flag); CHECK(s_pending.waiting);
    flag.sequence = 0U; accept_frame(&flag); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_COMPLETE);
    CHECK(response.result.failed == 1U && response.sequence == UINT64_MAX - 1U);
    reset_task_test();
}

/* 普通心跳、进度、完整结果粘包；实际状态机收齐末尾CRC/帧尾后才交付。 */
static void test_result_stream(void)
{
    reset_task_test();
    rk_request_t request = {17U, 703U, 1000U, 0U};
    rk_response_t response;
    CHECK(rk_link_request(&request) == 0); process_request();
    uint64_t sequence = s_pending.sequence;
    tlv_frame_t frames[3];
    frame_init(&frames[0]); frames[0].sequence = sequence;
    frames[1] = progress_frame(sequence, 17U, 72U);
    frames[2] = dual_die_frame(3U, 1U, 1U, UINT64_MAX, 5645U);
    frames[2].sequence = sequence;
    tlv_sm_t stream; tlv_sm_init(&stream);
    uint8_t wire[TLV_FRAME_MAX];
    for (unsigned n = 0U; n < 3U; ++n) {
        unsigned length = tlv_pack_frame(&frames[n], sequence, wire, sizeof(wire));
        CHECK(length == (n == 0U ? 16U : n == 1U ? 27U : 73U));
        for (unsigned i = 0U; i < length; ++i) {
            if (tlv_sm_feed(&stream, wire[i]) == 1) {
                tlv_frame_t parsed;
                CHECK(tlv_sm_get_frame(&stream, &parsed) >= 0);
                accept_frame(&parsed);
            }
            if (n == 2U && i + 1U < length) CHECK(s_pending.waiting);
        }
        process_request();
        if (n < 2U) CHECK(s_pending.waiting && rk_link_receive(&response) == 0);
    }
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_COMPLETE);
    CHECK(response.result.failure_count == 2U && response.result.failures[0].die == 1U);
    CHECK(response.result.failures[1].error_address == 1U && response.result.elapsed_ms == 5645U);
    CHECK(s_pending.progress_percent == 72U && s_pending.progress_sequence == sequence);

    /* 完整包到达前已超时：迟到结果只恢复状态，绝不再报一次COMPLETE。 */
    reset_task_test();
    CHECK(rk_link_request(&request) == 0); process_request();
    frames[2] = task_result_frame(s_pending.sequence, 17U, 0U);
    tick = 1000U; process_request(); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_TIMEOUT);
    CHECK(s_pending.waiting);
    accept_frame(&frames[2]); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_RECOVERED);
    CHECK(rk_link_receive(&response) == 0 && !s_pending.waiting);
    reset_task_test();
}

/* 真实算法可运行十几分钟；0必须无限等待，与串口字节间超时判定互不影响。 */
static void test_unlimited_result_wait(void)
{
    reset_task_test();
    rk_request_t request = {3U, 501U, 0U, 0U};
    rk_response_t response;
    CHECK(rk_link_request(&request) == 0); process_request();
    CHECK(sent == 1U && s_pending.waiting && !s_pending.timed_out);
    tick = 11000U; process_request();
    CHECK(sent == 1U && s_pending.waiting && rk_link_receive(&response) == 0);
    tick = 20U * 60U * 1000U; process_request();
    CHECK(sent == 1U && s_pending.waiting && !s_pending.timed_out);
    tlv_frame_t result = task_result_frame(s_pending.sequence, 3U, UINT64_C(0x1234567890));
    accept_frame(&result); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_COMPLETE);
    CHECK(response.cookie == 501U && response.result.failures[0].error_address == UINT64_C(0x1234567890));
    accept_frame(&result); process_request(); CHECK(rk_link_receive(&response) == 0);

    /* 无限等待跨tick回绕仍不误判超时，不重发；最终结果只交付一次。 */
    tick = UINT32_MAX - 5U;
    CHECK(rk_link_request(&request) == 0); process_request();
    tick = 10U; process_request();
    CHECK(sent == 2U && s_pending.waiting && !s_pending.timed_out);
    result = task_result_frame(s_pending.sequence, 3U, 0U);
    accept_frame(&result); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_COMPLETE);
    CHECK(rk_link_receive(&response) == 0);
}

/* 两个待交付事件只能按TIMEOUT→RECOVERED入队；队列满不能覆盖或抢发下个命令。
 * 同一套边界分别注入迟到结果和B1掉电，后者还应逐条取消尚未发送的请求。
 */
static void test_timeout_recovery_backpressure(void)
{
    unsigned modes = 1U;
#if defined(B1_RK_UART)
    modes = 2U;
#endif
    for (unsigned mode = 0U; mode < modes; ++mode) {
        reset_task_test();
        rk_request_t request = {3U, 601U, 1000U, 0U};
        rk_response_t response, filler = {.cookie = 999U, .status = RK_RESPONSE_TX_FAILED};
        CHECK(rk_link_request(&request) == 0); process_request();
        uint64_t sequence = s_pending.sequence;
        request.cookie = 602U; CHECK(rk_link_request(&request) == 0);
        for (unsigned i = 0U; i < 8U; ++i) CHECK(xQueueSend(rkResponseQueue, &filler, 0U) == pdTRUE);
        tick = 999U; process_request(); CHECK(!s_pending.timed_out && sent == 1U);
        tick = 1000U; process_request(); process_request();
        CHECK(s_pending.timed_out && s_pending.delivery && s_pending.waiting);
        CHECK(s_pending.response.status == RK_RESPONSE_TIMEOUT);
        tlv_frame_t result = task_result_frame(sequence, 3U, 0U);
        if (mode == 0U) {
            accept_frame(&result); accept_frame(&result); process_request();
        }
#if defined(B1_RK_UART)
        else {
            power_on = 0; process_power_off(); process_power_off();
        }
#endif
        CHECK(!s_pending.waiting && s_pending.recovered && s_pending.delivery && sent == 1U);
        CHECK(s_pending.response.status == RK_RESPONSE_TIMEOUT && requests.count == 1U);

        /* 只空出一格：先放TIMEOUT，RECOVERED仍保留，不能消费后续请求。 */
        CHECK(rk_link_receive(&response) == 1 && response.cookie == 999U);
        if (mode == 0U) process_request();
#if defined(B1_RK_UART)
        else process_power_off();
#endif
        CHECK(s_pending.delivery && !s_pending.recovered && sent == 1U && requests.count == 1U);
        CHECK(s_pending.response.status == RK_RESPONSE_RECOVERED);
        for (unsigned i = 0U; i < 7U; ++i) {
            CHECK(rk_link_receive(&response) == 1 && response.cookie == 999U);
        }
        CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_TIMEOUT);
        CHECK(response.cookie == 601U && response.sequence == sequence);
        CHECK(cleared(&response.result, sizeof(response.result)));
        CHECK(rk_link_receive(&response) == 0);

        if (mode == 0U) process_request();
#if defined(B1_RK_UART)
        else process_power_off();
#endif
        CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_RECOVERED);
        CHECK(response.cookie == 601U && response.sequence == sequence);
        CHECK(cleared(&response.result, sizeof(response.result)));
        CHECK(rk_link_receive(&response) == 0);
        if (mode == 0U) {
            CHECK(sent == 2U && s_pending.waiting && requests.count == 0U);
            accept_frame(&result); process_request();
            CHECK(s_pending.waiting && rk_link_receive(&response) == 0);
            result = task_result_frame(s_pending.sequence, 3U, 0U);
            accept_frame(&result); process_request();
            CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_COMPLETE);
            CHECK(response.cookie == 602U);
        }
#if defined(B1_RK_UART)
        else {
            process_power_off();
            CHECK(sent == 1U && !s_pending.waiting && requests.count == 0U);
            CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_POWER_OFF);
            CHECK(response.cookie == 602U && response.sequence == 0U);
            process_power_off();
        }
#endif
        CHECK(rk_link_receive(&response) == 0);
    }

    /* 到期后先收到结果、尚未来得及轮询超时，也只能报TIMEOUT与恢复。 */
    reset_task_test();
    rk_request_t request = {3U, 603U, 1000U, 0U};
    rk_response_t response;
    CHECK(rk_link_request(&request) == 0); process_request();
    tick = 1000U;
    tlv_frame_t result = task_result_frame(s_pending.sequence, 3U, 0U);
    accept_frame(&result); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_TIMEOUT);
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_RECOVERED);
    accept_frame(&result); process_request(); CHECK(rk_link_receive(&response) == 0);
}

/* 毫秒桩核对发送边界、失败不忙重试、错过周期不追赶和tick回绕。
 * V2与独立B1均固定验收1000ms，防止两工程的心跳周期不同步。
 */
static void test_ready_heartbeat(void)
{
    reset_task_test();
    CHECK(rk_link_ready_begin(0U) == -1 && rk_link_ready_poll(0U) == -1);
    uint64_t old_tx = rk_link_next_sequence();
    CHECK(rk_link_ready_begin(101U) == 0);
    tlv_frame_t frame = {0};
    rk_link_ready_note_tx(old_tx); /* 布防前分配、布防后才返回的旧发送不能确认。 */
    frame.sequence = old_tx;
    accept_frame(&frame); CHECK(rk_link_ready_poll(101U) == 0);
    frame.sequence = 900U;
    accept_frame(&frame); CHECK(rk_link_ready_poll(101U) == 0);
    TickType_t last = 0U;
    tick = 1000U; process_heartbeat(&last);
    uint64_t first = tx_log[0].sequence;
    frame.sequence = first - 1U;
    accept_frame(&frame); CHECK(rk_link_ready_poll(101U) == 0);
    frame.sequence = first + 1U;
    accept_frame(&frame); CHECK(rk_link_ready_poll(101U) == 0);
    frame.sequence = first;
    frame.count = 1U; frame.units[0].type = TLV_TYPE_HEARTBEAT; frame.units[0].data_len = 1U;
    accept_frame(&frame); CHECK(rk_link_ready_poll(101U) == 0);
    frame.count = 2U; frame.units[0].type = 0x5001U; frame.units[1].type = 0x3016U;
    accept_frame(&frame); CHECK(rk_link_ready_poll(101U) == 0);
    frame = task_result_frame(first, 17U, 0U);
    accept_frame(&frame); CHECK(rk_link_ready_poll(101U) == 0);
    /* 发送失败的Seq不得放行；正常心跳须先通过线上两级CRC检查。 */
    tx_failure = 1; tick = 2000U; process_heartbeat(&last); tx_failure = 0;
    uint8_t wire[TLV_HEARTBEAT_FRAME_SIZE];
    CHECK(tlv_pack_heartbeat(tx_log[1].sequence, wire, sizeof(wire)) == 16);
    CHECK(tlv_unpack(wire, sizeof(wire), &frame) == 0);
    accept_frame(&frame); CHECK(rk_link_ready_poll(101U) == 0);
    CHECK(tlv_pack_heartbeat(first, wire, sizeof(wire)) == 16);
    wire[13] ^= 1U;
    CHECK(tlv_unpack(wire, sizeof(wire), &frame) < 0);
    CHECK(rk_link_ready_poll(101U) == 0);
    wire[13] ^= 1U;
    CHECK(tlv_unpack(wire, sizeof(wire), &frame) == 0);
    accept_frame(&frame);
    CHECK(rk_link_ready_poll(101U) == 1); /* 不需要已有算法在途。 */
    CHECK(rk_link_ready_poll(100U) == -1);
    CHECK(rk_link_ready_begin(102U) == 0);
    accept_frame(&frame); CHECK(rk_link_ready_poll(102U) == 0);
    tick = 3000U; process_heartbeat(&last);
    accept_frame(&frame); CHECK(rk_link_ready_poll(102U) == 0);
    frame.sequence = tx_log[2].sequence;
    accept_frame(&frame); CHECK(rk_link_ready_poll(102U) == 1);
    /* 窗口允许滞后和重复；兼容空Value心跳但不接受未来Seq。 */
    CHECK(rk_link_ready_begin(103U) == 0);
    tick = 4000U; process_heartbeat(&last);
    tick = 5000U; process_heartbeat(&last);
    frame.sequence = tx_log[4].sequence + 1U;
    accept_frame(&frame); CHECK(rk_link_ready_poll(103U) == 0);
    frame.count = 1U; frame.units[0].type = TLV_TYPE_HEARTBEAT; frame.units[0].data_len = 0U;
    frame.sequence = tx_log[3].sequence;
    accept_frame(&frame); accept_frame(&frame);
    CHECK(rk_link_ready_poll(103U) == 1);
}

static void test_heartbeat(void)
{
    const TickType_t interval = pdMS_TO_TICKS(RK_HEARTBEAT_INTERVAL_MS);
    CHECK(interval == 1000U);
    reset_task_test();
    TickType_t last = 100U;
    tick = 100U + interval - 1U; process_heartbeat(&last); CHECK(tx_attempts == 0U && last == 100U);
    tick = 100U + interval; process_heartbeat(&last); CHECK(sent == 1U && last == tick);
    CHECK(tx_log[0].type == TLV_TYPE_HEARTBEAT);
    tx_failure = 1;
    tick = 100U + interval * 2U; process_heartbeat(&last); CHECK(sent == 1U && tx_attempts == 2U);
    process_heartbeat(&last); CHECK(tx_attempts == 2U && last == tick);
    tx_failure = 0;
    tick = 100U + interval * 3U - 1U; process_heartbeat(&last); CHECK(tx_attempts == 2U);
    tick = 100U + interval * 3U; process_heartbeat(&last); CHECK(sent == 2U && tx_attempts == 3U);
    tick = 100U + interval * 10U; process_heartbeat(&last); process_heartbeat(&last);
    CHECK(sent == 3U && tx_attempts == 4U && last == tick);
    for (unsigned i = 1U; i < tx_attempts; ++i) {
        CHECK(tx_log[i].sequence == tx_log[i - 1U].sequence + 1U);
    }
    last = UINT32_MAX - (interval / 2U - 1U);
    tick = interval / 2U - 1U; process_heartbeat(&last); CHECK(tx_attempts == 4U);
    tick = interval / 2U; process_heartbeat(&last); CHECK(tx_attempts == 5U && last == tick);

    reset_task_test();
    rk_request_t request = {3U, 301U, 5000U, 0U};
    CHECK(rk_link_request(&request) == 0); process_request();
    CHECK(s_pending.waiting && tx_log[0].type == TLV_TYPE_CMD);
    rk_pending_t pending = s_pending;
    last = 0U; tick = interval; process_heartbeat(&last);
    pending.last_tx_sequence = tx_log[1].sequence;
    CHECK(sent == 2U && memcmp(&pending, &s_pending, sizeof(pending)) == 0);
    CHECK(tx_log[1].type == TLV_TYPE_HEARTBEAT);
    CHECK(tx_log[1].sequence == tx_log[0].sequence + 1U);
    tlv_frame_t result = task_result_frame(tx_log[1].sequence, 3U, 0U);
    accept_frame(&result); process_request();
    rk_response_t response;
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_COMPLETE);
    CHECK(rk_link_request(&request) == 0); process_request();
    CHECK(tx_log[2].type == TLV_TYPE_CMD && tx_log[2].sequence == tx_log[1].sequence + 1U);
#if defined(B1_RK_UART)
    /* ON判断后发生掉电时，发送前的二次检查也必须拦住心跳。 */
    power_on = 0; tick = 7000U; process_heartbeat(&last);
    CHECK(tx_attempts == 3U && last == 7000U);
    power_on = PWR_CTRL_STATE_ON;
    tick = 7000U + interval - 1U; process_heartbeat(&last); CHECK(tx_attempts == 3U);
    tick = 7000U + interval; process_heartbeat(&last); CHECK(tx_attempts == 4U);
#endif
}

/* 执行真实任务循环，由延时桩在指定轮次退出；不向生产代码添加测试钩子。
 * 持续RX或GAP每轮都耗尽8块配额，仍须推进超时、心跳、任务存活上报和调度让步。
 */
static void run_task_iterations(void)
{
    if (setjmp(task_exit) == 0) StartRkTask(NULL);
}

/* 真实任务处理CRC正确但进度越界的线帧：记录TLV拒绝，不虚构已解包的Seq。 */
static void test_rx_tlv_diagnostics(void)
{
    reset_task_test();
    memset(rx_wire, 0, sizeof(rx_wire));
    rx_wire[13] = 0x30U; rx_wire[14] = 0x16U;
    rx_wire[15] = 0U; rx_wire[16] = 1U; rx_wire[17] = 101U;
    rx_wire_length = seal(rx_wire, 5U);
    rx_mode = 3;
    task_iterations = 1U;
    run_task_iterations();
    rk_rx_diagnostics_t diag;
    CHECK(rk_link_get_rx_diagnostics(&diag) == 0 && diag.rejected_total == 8U);
    CHECK(diag.counts[RK_RX_REJECT_TLV] == 8U && diag.last_reason == RK_RX_REJECT_TLV);
    CHECK(!diag.frame_valid && !diag.waiting && diag.received_sequence == 0U && diag.tlv_count == 0U);
    CHECK(!s_pending.waiting && responses.count == 0U);
    reset_task_test();
}

static void test_task_fairness(void)
{
    const unsigned interval = RK_HEARTBEAT_INTERVAL_MS;
    for (int mode = 1; mode <= 2; ++mode) {
        reset_task_test();
        rx_mode = mode;
        task_iterations = interval / 10U + 2U;
        rk_request_t request = {3U, 401U, 100U, 0U};
        CHECK(rk_link_request(&request) == 0);
        run_task_iterations();
        CHECK(delay_calls == task_iterations && health_calls == task_iterations && rx_calls == task_iterations * 8U);
        CHECK(tx_attempts == 2U && tx_log[0].type == TLV_TYPE_CMD);
        CHECK(tx_log[1].type == TLV_TYPE_HEARTBEAT && tx_log[1].tick == interval);
        CHECK(s_pending.waiting && s_pending.timed_out);
        rk_response_t response;
        CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_TIMEOUT);
        CHECK(rk_link_receive(&response) == 0);
    }
#if defined(B1_RK_UART)
    /* OFF期间跨过一个心跳周期也不能发送；ON恢复只从最近OFF轮询重新计时。
     * OFF按20ms休眠、ON按10ms休眠；首心跳为最近OFF轮询时刻加配置周期。
     */
    for (int mode = 1; mode <= 2; ++mode) {
        reset_task_test();
        rk_request_t request = {3U, 402U, 5000U, 0U};
        CHECK(rk_link_request(&request) == 0);
        power_on = 0;
        rx_mode = mode;
        resume_power_after = interval / 20U + 2U;
        task_iterations = resume_power_after + interval / 10U;
        run_task_iterations();
        CHECK(delay_calls == task_iterations && health_calls == task_iterations && rx_calls == task_iterations * 8U);
        CHECK(tx_attempts == 1U && tx_log[0].type == TLV_TYPE_HEARTBEAT);
        CHECK(tx_log[0].tick == (resume_power_after - 1U) * 20U + interval && !s_pending.waiting);
        rk_response_t response;
        CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_POWER_OFF);
        CHECK(rk_link_receive(&response) == 0);
    }
#endif
    reset_task_test();
}
/* 构造一个完整双颗粒包；mask选择明细，默认置整体FAIL，PASS用例另行清零。 */
static tlv_frame_t dual_die_frame(uint8_t mask, uint8_t reverse,
                                  uint64_t first_address, uint64_t second_address,
                                  uint64_t elapsed_ms)
{
    tlv_frame_t frame;
    frame_init(&frame); frame.sequence = 0x30U;
    uint8_t command[] = {17U, 0U}, failed = mask != 0U, value[8];
    CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_CMD, command, 2U) == 0);
    CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_FAIL, &failed, 1U) == 0);
    for (uint8_t index = 0U; index < 2U; ++index) {
        uint8_t die = reverse ? 1U - index : index;
        if ((mask & (1U << die)) == 0U) continue;
        uint64_t address = die == 0U ? first_address : second_address;
        for (unsigned byte = 0U; byte < 8U; ++byte) value[byte] = (uint8_t)(address >> (8U * byte));
        CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_ADDR, value, 8U) == 0);
        CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_DIE, &die, 1U) == 0);
    }
    for (unsigned byte = 0U; byte < 8U; ++byte) value[byte] = (uint8_t)(elapsed_ms >> (8U * byte));
    CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_TIME, value, 8U) == 0);
    return frame;
}

/* CRC黄金值离线独立计算；不能用生产打包器生成后又只拿同一实现验证。 */
static void test_dual_die_final_golden(void)
{
    static const uint8_t golden[][73] = {
        {0x5A,0x00,0x17,0x11,0xDD,0,0,0,0,0,0,0,0x30,
         0x50,0x01,0,2,0x11,0,0x10,0x17,0,1,0,
         0x90,0x02,0,8,0x0D,0x16,0,0,0,0,0,0,0x68,0x0B,0xA5},
        {0x5A,0x00,0x28,0x51,0xCD,0,0,0,0,0,0,0,0x30,
         0x50,0x01,0,2,0x11,0,0x10,0x17,0,1,1,
         0x90,0x01,0,8,0xF0,0xDE,0xBC,0x9A,0x78,0x56,0x34,0x12,
         0x30,0x01,0,1,0,0x90,0x02,0,8,0x0D,0x16,0,0,0,0,0,0,0x48,0x31,0xA5},
        {0x5A,0x00,0x39,0x91,0xC1,0,0,0,0,0,0,0,0x30,
         0x50,0x01,0,2,0x11,0,0x10,0x17,0,1,1,
         0x90,0x01,0,8,0x10,0x32,0x54,0x76,0x98,0xBA,0xDC,0xFE,0x30,0x01,0,1,1,
         0x90,0x01,0,8,0xF0,0xDE,0xBC,0x9A,0x78,0x56,0x34,0x12,0x30,0x01,0,1,0,
         0x90,0x02,0,8,0x0D,0x16,0,0,0,0,0,0,0xAA,0xA9,0xA5}
    };
    const unsigned lengths[] = {39U, 56U, 73U};
    const uint8_t masks[] = {0U, 1U, 3U};
    for (unsigned kind = 0U; kind < 3U; ++kind) {
        uint8_t wire[TLV_FRAME_MAX];
        tlv_frame_t frame = dual_die_frame(masks[kind], 1U,
            UINT64_C(0x123456789ABCDEF0), UINT64_C(0xFEDCBA9876543210), 5645U);
        CHECK(tlv_pack_frame(&frame, 0x30U, wire, sizeof(wire)) == (int)lengths[kind]);
        CHECK(memcmp(wire, golden[kind], lengths[kind]) == 0);
        CHECK(tlv_unpack(golden[kind], lengths[kind], &frame) == (int)(3U + 2U * kind));
        rk_final_result_t result;
        CHECK(rk_final_result_decode(&frame, &result) == 0);
        CHECK(result.command_id == 17U && result.failed == (kind != 0U));
        CHECK(result.failure_count == kind && result.elapsed_ms == 5645U);
        if (kind == 1U) CHECK(result.failures[0].die == 0U && result.failures[0].error_address == UINT64_C(0x123456789ABCDEF0));
        if (kind == 2U) {
            CHECK(result.failures[0].die == 1U && result.failures[0].error_address == UINT64_C(0xFEDCBA9876543210));
            CHECK(result.failures[1].die == 0U && result.failures[1].error_address == UINT64_C(0x123456789ABCDEF0));
        }
        for (unsigned i = 0U; i < lengths[kind]; ++i) {
            memcpy(wire, golden[kind], lengths[kind]); wire[i] ^= 1U;
            CHECK(tlv_unpack(wire, lengths[kind], &frame) == -1);
            CHECK(tlv_unpack(golden[kind], i, &frame) == -1);
        }
        /* 每个切分点都走实际字节流状态机；50ms分段未超过既定100ms门限。 */
        for (unsigned split = 1U; split < lengths[kind]; ++split) {
            tlv_sm_t stream;
            tlv_sm_init(&stream); tick = 0U;
            for (unsigned byte = 0U; byte < lengths[kind]; ++byte) {
                if (byte == split) tick += 50U;
                int ready = tlv_sm_feed(&stream, golden[kind][byte]);
                CHECK(ready == (byte + 1U == lengths[kind]));
            }
            CHECK(tlv_sm_get_frame(&stream, &frame) == (int)(3U + 2U * kind));
            rk_final_result_t streamed;
            CHECK(rk_final_result_decode(&frame, &streamed) == 0);
            CHECK(memcmp(&streamed, &result, sizeof(result)) == 0);
        }
    }
}

static void final_result_rejected(const tlv_frame_t *frame)
{
    rk_final_result_t result;
    memset(&result, 0xA5, sizeof(result));
    CHECK(rk_final_result_decode(frame, &result) == -1);
    CHECK(cleared(&result, sizeof(result)));
}

static void test_dual_die_final_rules(void)
{
    const uint64_t boundaries[] = {1U, UINT64_MAX};
    for (uint8_t mask = 0U; mask < 4U; ++mask) {
        for (uint8_t reverse = 0U; reverse < 2U; ++reverse) {
            for (unsigned edge = 0U; edge < 2U; ++edge) {
                tlv_frame_t frame = dual_die_frame(mask, reverse, boundaries[edge], boundaries[1U - edge], boundaries[edge]);
                rk_final_result_t result;
                CHECK(rk_final_result_decode(&frame, &result) == 0);
                CHECK(result.failed == (mask != 0U) && result.failure_count == ((mask & 1U) != 0U) + ((mask & 2U) != 0U));
                CHECK(result.elapsed_ms == boundaries[edge]);
                uint8_t decoded_mask = 0U;
                for (uint8_t i = 0U; i < result.failure_count; ++i) {
                    uint8_t die = result.failures[i].die;
                    CHECK(die < 2U && !(decoded_mask & (1U << die)));
                    decoded_mask |= (uint8_t)(1U << die);
                    CHECK(result.failures[i].error_address == boundaries[die == 0U ? edge : 1U - edge]);
                }
                CHECK(decoded_mask == mask);
                /* 必需标量可换序，地址/编号保持相邻时结果不能改变。 */
                tlv_unit_t first = frame.units[0];
                frame.units[0] = frame.units[frame.count - 1U]; frame.units[frame.count - 1U] = first;
                rk_final_result_t reordered;
                CHECK(rk_final_result_decode(&frame, &reordered) == 0);
                CHECK(memcmp(&result, &reordered, sizeof(result)) == 0);
                frame.payload[frame.units[1].data_off] = mask == 0U ? 1U : 0U;
                final_result_rejected(&frame); /* 全局FAIL与失败明细数量矛盾。 */
            }
        }
    }
    tlv_frame_t good = dual_die_frame(3U, 0U, 1U, UINT64_MAX, 0U), frame;
    CHECK(rk_final_result_decode(&good, NULL) == -1);
    final_result_rejected(NULL);
    for (unsigned count = 0U; count <= TLV_MAX_PER_FRAME + 1U; ++count) {
        if (count == 3U || count == 5U || count == 7U) continue;
        frame_init(&frame);
        for (unsigned i = 0U; i < count && i < TLV_MAX_PER_FRAME; ++i)
            CHECK(tlv_pack(&frame, 0x7777U, NULL, 0U) == 0);
        if (count > TLV_MAX_PER_FRAME) frame.count = count;
        final_result_rejected(&frame);
    }
    /* 对每个字段分别注入未知类型、错误宽度和失效偏移。 */
    for (uint8_t i = 0U; i < good.count; ++i) {
        frame = good; frame.units[i].type = 0x7777U; final_result_rejected(&frame);
        frame = good; --frame.units[i].data_len; final_result_rejected(&frame);
        frame = good; frame.units[i].data_off = UINT16_MAX; final_result_rejected(&frame);
    }
    frame = good; frame.payload_used = TLV_PAYLOAD_MAX + 1U; final_result_rejected(&frame);
    frame = good; frame.payload[frame.units[1].data_off] = 2U; final_result_rejected(&frame);
    frame = good; frame.payload[frame.units[5].data_off] = 0U; final_result_rejected(&frame);
    frame = good; frame.payload[frame.units[3].data_off] = 2U; final_result_rejected(&frame);
    frame = good; frame.payload[frame.units[3].data_off] = 255U; final_result_rejected(&frame);
    frame = good; frame.units[1] = frame.units[0]; final_result_rejected(&frame);
    frame = good; frame.units[0] = frame.units[1]; final_result_rejected(&frame);
    frame = good; frame.units[0] = frame.units[6]; final_result_rejected(&frame);
    frame = good; frame.units[2] = good.units[3]; frame.units[3] = good.units[2]; final_result_rejected(&frame);
    frame = good; frame.units[3] = good.units[4]; frame.units[4] = good.units[3]; final_result_rejected(&frame);
    frame = good; frame.units[5] = good.units[6]; frame.units[6] = good.units[5]; final_result_rejected(&frame);
    /* 心跳、进度和原四详情/独立FAIL都不是一个完整的双颗粒最终包。 */
    frame_init(&frame); final_result_rejected(&frame);
    frame = progress_frame(1U, 17U, 100U); final_result_rejected(&frame);
    frame = scalar_frame(1U, TLV_TYPE_RESULT_FAIL, 0U); final_result_rejected(&frame);
    frame = result_frame(1U, 17U, 0U); final_result_rejected(&frame);
    uint8_t zero[8] = {0U};
    const uint16_t extras[] = {TLV_TYPE_HEARTBEAT, TLV_TYPE_CMD, TLV_TYPE_PROGRESS};
    const uint16_t sizes[] = {0U, 2U, 1U};
    for (unsigned i = 0U; i < 3U; ++i) {
        frame = dual_die_frame(0U, 0U, 0U, 0U, 0U);
        CHECK(tlv_pack(&frame, extras[i], zero, sizes[i]) == 0);
        CHECK(tlv_pack(&frame, extras[i], zero, sizes[i]) == 0);
        final_result_rejected(&frame);
    }
    rk_final_result_t result;
    CHECK(rk_final_result_decode(&good, &result) == 0 && result.failure_count == 2U);
    frame = dual_die_frame(0U, 0U, 0U, 0U, 0U);
    CHECK(rk_final_result_decode(&frame, &result) == 0 && result.failure_count == 0U);
    CHECK(cleared(result.failures, sizeof(result.failures))); /* 不能残留上一包失败地址。 */
}

/* PASS兼容零地址明细；FAIL保留全部已列颗粒及原始地址，包括地址0。 */
static void test_explicit_die_results(void)
{
    static const uint8_t golden[] = {
        0x5A,0,0x28,0x51,0xCD,0,0,0,0,0,0,0,0x30,
        0x50,0x01,0,2,0x11,0,0x10,0x17,0,1,0,
        0x90,0x01,0,8,0,0,0,0,0,0,0,0,0x30,0x01,0,1,0,
        0x90,0x02,0,8,0x0D,0x16,0,0,0,0,0,0,0x8F,0x68,0xA5
    };
    tlv_frame_t frame;
    rk_final_result_t result;
    CHECK(tlv_unpack(golden, sizeof(golden), &frame) == 5);
    CHECK(rk_final_result_decode(&frame, &result) == 0);
    CHECK(result.command_id == 17U && result.elapsed_ms == 5645U);
    CHECK(result.failed == 0U && result.failure_count == 0U);
    CHECK(cleared(result.failures, sizeof(result.failures)));
    /* 五TLV PASS按每个切分点通过实际状态机，不能因零地址放宽CRC。 */
    for (unsigned split = 1U; split < sizeof(golden); ++split) {
        tlv_sm_t stream; tlv_sm_init(&stream); tick = 0U;
        CHECK(feed(&stream, golden, split) == 0);
        tick += 50U;
        CHECK(feed(&stream, golden + split, sizeof(golden) - split) == 1);
        CHECK(tlv_sm_get_frame(&stream, &frame) == 5);
        CHECK(rk_final_result_decode(&frame, &result) == 0 && result.failure_count == 0U);
    }
    uint8_t wire[sizeof(golden)];
    memcpy(wire, golden, sizeof(golden)); wire[29] ^= 1U;
    CHECK(tlv_unpack(wire, sizeof(golden), &frame) == -1);
    for (uint8_t pairs = 0U; pairs < 4U; ++pairs) {
        for (uint8_t nonzero = 0U; nonzero < 4U; ++nonzero) {
            if ((nonzero & ~pairs) != 0U) continue;
            for (uint8_t failed = 0U; failed <= 1U; ++failed) {
                for (uint8_t reverse = 0U; reverse < 2U; ++reverse) {
                    frame = dual_die_frame(pairs, reverse, (nonzero & 1U) ? 1U : 0U,
                                           (nonzero & 2U) ? UINT64_MAX : 0U, 0U);
                    frame.payload[frame.units[1].data_off] = failed;
                    if ((failed && pairs == 0U) || (!failed && nonzero != 0U)) {
                        final_result_rejected(&frame);
                        continue;
                    }
                    CHECK(rk_final_result_decode(&frame, &result) == 0);
                    CHECK(result.failed == failed);
                    CHECK(result.failure_count == (failed ? ((pairs & 1U) != 0U) + ((pairs & 2U) != 0U) : 0));
                    uint8_t decoded_mask = 0U;
                    for (uint8_t i = 0U; i < result.failure_count; ++i) {
                        uint8_t die = result.failures[i].die;
                        decoded_mask |= (uint8_t)(1U << die);
                        uint64_t expected = (nonzero & (1U << die)) ? (die ? UINT64_MAX : 1U) : 0U;
                        CHECK(result.failures[i].error_address == expected);
                    }
                    CHECK(decoded_mask == (failed ? pairs : 0U));
                    if (!failed) CHECK(cleared(result.failures, sizeof(result.failures)));
                    /* FAIL标量可在明细后面，不能依赖遍历时是否已经读到1017。 */
                    tlv_unit_t flag = frame.units[1];
                    frame.units[1] = frame.units[frame.count - 1U];
                    frame.units[frame.count - 1U] = flag;
                    rk_final_result_t reordered;
                    CHECK(rk_final_result_decode(&frame, &reordered) == 0);
                    CHECK(memcmp(&result, &reordered, sizeof(result)) == 0);
                    reset_task_test();
                    rk_request_t request = {17U, 901U, 0U, 0U};
                    rk_response_t response;
                    CHECK(rk_link_request(&request) == 0); process_request();
                    frame.sequence = s_pending.sequence;
                    accept_frame(&frame); accept_frame(&frame); process_request();
                    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_COMPLETE);
                    CHECK(memcmp(&response.result, &result, sizeof(result)) == 0);
                    CHECK(!s_pending.waiting && rk_link_receive(&response) == 0);
                }
            }
        }
    }
    /* 全PASS及FAIL帧都禁止重复die，不能因零地址跳过查重。 */
    for (unsigned mixed = 0U; mixed < 2U; ++mixed) {
        frame = dual_die_frame(3U, 0U, 0U, mixed ? 1U : 0U, 0U);
        frame.payload[frame.units[1].data_off] = mixed;
        frame.payload[frame.units[5].data_off] = 0U;
        final_result_rejected(&frame);
    }
    reset_task_test();
}

/* RK被动回复：空闲确认ready、在途回复进度，MCU不回发造成应答循环。 */
static void test_reply_only_heartbeat(void)
{
    reset_task_test();
    TickType_t last = 0U;
    rk_response_t response;
    CHECK(rk_link_ready_begin(901U) == 0);
    tick = 1000U; process_heartbeat(&last);
    CHECK(tx_attempts == 1U);
    tlv_frame_t frame; frame_init(&frame); frame.sequence = tx_log[0].sequence;
    accept_frame(&frame); process_request(); process_heartbeat(&last);
    CHECK(rk_link_ready_poll(901U) == 1 && tx_attempts == 1U);
    rk_request_t request = {17U, 902U, 0U, 0U};
    CHECK(rk_link_request(&request) == 0); process_request();
    CHECK(tx_attempts == 2U);
    tick = 2000U; process_heartbeat(&last);
    frame = progress_frame(tx_log[2].sequence, 17U, 100U);
    accept_frame(&frame); process_request(); process_heartbeat(&last);
    CHECK(tx_attempts == 3U && s_pending.waiting && s_pending.progress_percent == 100U);
    CHECK(rk_link_receive(&response) == 0);
    /* 结果可在下一心跳前主动到达，不依赖额外普通心跳。 */
    tick += 10U;
    frame = task_result_frame(tx_log[2].sequence, 17U, 0U);
    accept_frame(&frame); process_request(); process_heartbeat(&last);
    CHECK(tx_attempts == 3U && !s_pending.waiting);
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_COMPLETE);
    reset_task_test();
}

/* 真实编码/任务覆盖：无回应时序不会占算法结果窗口，频率可在ON之前应答。 */
static void test_timing_and_frequency(void)
{
    uint8_t wire[TLV_FREQUENCY_FRAME_SIZE];
    tlv_frame_t frame;
    rk_response_t event;
    reset_task_test();
    CHECK(tlv_pack_timing(0x27U, 1U, 1U, wire, sizeof(wire)) == 0);
    CHECK(tlv_pack_timing(0x2BU, UINT32_C(0x78563412), 7U, wire, sizeof(wire)) == 30);
    CHECK(tlv_unpack(wire, 30U, &frame) == 2);
    static const uint8_t timing_tlv[] = {0x50,0,0,2,0x2B,0,0x70,0x1B,0,4,0x12,0x34,0x56,0x78};
    CHECK(memcmp(wire + 13U, timing_tlv, sizeof(timing_tlv)) == 0 && frame.sequence == 7U);
    memset(wire, 0xCC, sizeof(wire));
    CHECK(tlv_pack_frequency(-1, 9U, wire, 33U) == 0 && wire[0] == 0xCC);
    CHECK(tlv_pack_frequency(-1, 9U, wire, sizeof(wire)) == 34);
    static const uint8_t freq_tlv[] = {0x50,1,0,2,0x35,0,0x80,0x1C,0,8,255,255,255,255,255,255,255,255};
    CHECK(memcmp(wire + 13U, freq_tlv, sizeof(freq_tlv)) == 0);
    CHECK(tlv_unpack(wire, 34U, &frame) == 2 && frame.sequence == 9U);
    CHECK(tlv_pack_frequency(INT64_MIN, 10U, wire, sizeof(wire)) == 34 && wire[30] == 0x80);
    CHECK(tlv_unpack(wire, 34U, &frame) == 2 && !rk_is_frequency_query(&frame));
    CHECK(tlv_pack_cmd(0x35U, NULL, 0U, 0x35U, wire, sizeof(wire)) == 22);
    CHECK(tlv_unpack(wire, 22U, &frame) == 1 && rk_is_frequency_query(&frame));
    rk_link_frequency_begin(801U, INT64_C(9007199254740993));
    accept_frame(&frame);
    CHECK(tx_attempts == 1U && tx_log[0].type == TLV_TYPE_RESULT_CMD && tx_log[0].command == 0x35U);
    CHECK(tx_log[0].value == UINT64_C(9007199254740993) && rk_link_frequency_status(801U) == 1);
    CHECK(!s_pending.waiting && rk_link_receive(&event) == 0);
    rk_link_frequency_end(800U);
    CHECK(rk_link_frequency_get(NULL) == INT64_C(9007199254740993));
    rk_link_frequency_end(801U);
    accept_frame(&frame);
    CHECK(tx_log[1].value == UINT64_MAX && tx_log[1].sequence == tx_log[0].sequence + 1U);
    rk_link_frequency_begin(802U, 1600);
    tx_failure = 1; accept_frame(&frame);
    CHECK(rk_link_frequency_status(802U) == -1 && !s_pending.waiting);
    reset_task_test();

    rk_request_t setting = {0x25U, 900U, 0U, UINT32_MAX};
    CHECK(rk_link_request(&setting) == 0); process_request();
    CHECK(!s_pending.waiting && tx_log[0].command == 0x25U && tx_log[0].value == UINT32_MAX);
    /* 结果队列满时保留SENT，不提前领取后续设置。 */
    responses.count = 8U;
    setting.command_id = 0x26U; setting.parameter_value = 0U;
    CHECK(rk_link_request(&setting) == 0); tick += 20U; process_request();
    CHECK(requests.count == 1U && tx_attempts == 1U && s_pending.delivery);
    responses.count = 0U; process_request();
    CHECK(rk_link_receive(&event) == 1 && event.status == RK_RESPONSE_SENT);
    CHECK(tx_attempts == 2U && tx_log[1].command == 0x26U && tx_log[1].value == 0U);
    process_request(); CHECK(rk_link_receive(&event) == 1 && event.status == RK_RESPONSE_SENT);
    rk_request_t algorithm = {17U, 901U, 0U, 0U};
    CHECK(rk_link_request(&algorithm) == 0); process_request(); CHECK(tx_attempts == 2U);
    tick += 19U; process_request(); CHECK(tx_attempts == 2U);
    ++tick; process_request(); CHECK(tx_attempts == 3U && s_pending.waiting);
    frame = progress_frame(s_pending.sequence, 0x26U, 100U);
    accept_frame(&frame); CHECK(s_pending.waiting && !s_pending.progress_valid);
    frame = task_result_frame(s_pending.sequence, 0x26U, 0U);
    accept_frame(&frame); CHECK(s_pending.waiting && rk_link_receive(&event) == 0);
    frame = task_result_frame(s_pending.sequence, 17U, 0U);
    accept_frame(&frame); process_request();
    CHECK(rk_link_receive(&event) == 1 && event.status == RK_RESPONSE_COMPLETE);
    reset_task_test();
    tx_failure = 1; CHECK(rk_link_request(&setting) == 0); process_request(); process_request();
    CHECK(tx_attempts == 1U && rk_link_receive(&event) == 1 && event.status == RK_RESPONSE_TX_FAILED);
    reset_task_test();
#if defined(B1_RK_UART)
    for (int state = PWR_CTRL_STATE_OFF; state <= PWR_CTRL_STATE_FAULT; ++state) {
        reset_task_test(); power_on = state;
        rk_link_frequency_begin(990U, 1234);
        CHECK(rk_link_ready_begin(991U) == 0);
        task_iterations = 1U; rx_mode = 3;
        rx_wire_length = (unsigned)tlv_pack_cmd(0x35U, NULL, 0U, 0x35U, rx_wire, sizeof(rx_wire));
        run_task_iterations();
        int allowed = state == PWR_CTRL_STATE_WAIT_FIRST_RAIL || state == PWR_CTRL_STATE_WAIT_SECOND_RAIL ||
                      state == PWR_CTRL_STATE_ON;
        CHECK(tx_attempts == (allowed ? 8U : 0U));
        CHECK(rk_link_frequency_status(990U) == (allowed ? 1 : 0));
        CHECK(rk_link_ready_poll(991U) == 0 && requests.count == 0U);
        if (allowed) CHECK(tx_log[0].value == 1234U);
    }
#endif
    reset_task_test();
    message("PASS: timing send-only sequencing, TX/backpressure, independent early frequency reply and power gates\n");
}

#if defined(TEST_B1_PROTOCOL_TRACE)
/* 从真实事务入口取日志，保留FAIL帧的非零/零地址明细及实际TLV顺序。 */
static void test_protocol_trace(void)
{
    tlv_frame_t frame;
    uint8_t wire[TLV_CMD_FRAME_SIZE];
    TickType_t last_heartbeat = 0U;
    reset_task_test();
    trace_capture_begin();
    rk_link_frequency_begin(71U, 2133);
    CHECK(rk_link_ready_begin(71U) == 0);
    CHECK(tlv_pack_cmd(RK_CMD_QUERY_FREQUENCY, NULL, 0U, UINT64_MAX, wire, sizeof(wire)) == sizeof(wire));
    CHECK(tlv_unpack(wire, sizeof(wire), &frame) == 1);
    accept_frame(&frame);
    CHECK(strstr(trace_capture_text(), "[RK RX] seq=18446744073709551615 5000[2]=3500\r\n"));
    CHECK(strstr(trace_capture_text(), "5001[2]=3500 801C[8]=5508000000000000 uart=queued\r\n"));
    tick = 1000U; process_heartbeat(&last_heartbeat);
    frame_init(&frame); frame.sequence = tx_log[1].sequence;
    accept_frame(&frame);
    CHECK(strstr(trace_capture_text(), "freqQuery=1\r\n"));
    trace_capture_begin(); accept_frame(&frame);
    CHECK(trace_capture_text()[0] == '\0');

    rk_link_frequency_begin(72U, 2133);
    CHECK(rk_link_ready_begin(72U) == 0);
    tick += 1000U; process_heartbeat(&last_heartbeat);
    frame.sequence = tx_log[2].sequence; accept_frame(&frame);
    CHECK(strstr(trace_capture_text(), "freqQuery=0\r\n"));
    CHECK(!strstr(trace_capture_text(), "3500"));
    rk_link_frequency_end(72U);
    reset_task_test();
    rk_request_t timing = {0x2BU, 1U, 0U, 38U};
    CHECK(rk_link_request(&timing) == 0); process_request();
    CHECK(strstr(trace_capture_text(), "5000[2]=2B00 701B[4]=26000000 uart=queued"));
    reset_task_test(); trace_capture_begin();
    tx_failure = 1;
    CHECK(rk_link_request(&timing) == 0); process_request();
    CHECK(strstr(trace_capture_text(), "5000[2]=2B00 701B[4]=26000000 uart=failed"));
    CHECK(!strstr(trace_capture_text(), "uart=queued"));

    for (unsigned include_zero_address = 0; include_zero_address <= 1; ++include_zero_address) {
        reset_task_test(); trace_capture_begin();
        rk_request_t algorithm = {5U, 2U, 0U, 0U};
        CHECK(rk_link_request(&algorithm) == 0); process_request();
        CHECK(strstr(trace_capture_text(), "5000[2]=0500 uart=queued"));
        frame = progress_frame(s_pending.sequence, 5U, 3U); accept_frame(&frame);
        frame = progress_frame(s_pending.sequence, 5U, 4U); accept_frame(&frame);
        frame = progress_frame(s_pending.sequence, 5U, 25U); accept_frame(&frame);
        CHECK(strstr(trace_capture_text(), "3016[1]=03") && !strstr(trace_capture_text(), "3016[1]=04"));
        CHECK(strstr(trace_capture_text(), "3016[1]=19"));
        size_t progress_length = strlen(trace_capture_text());
        frame = progress_frame(s_pending.sequence, 5U, 24U); accept_frame(&frame);
        frame = progress_frame(s_pending.sequence, 5U, 25U); accept_frame(&frame);
        CHECK(strlen(trace_capture_text()) == progress_length);
        frame = dual_die_frame(include_zero_address ? 3U : 1U, 0U, 0x200038U, 0U, 5497U);
        frame.sequence = s_pending.sequence;
        frame.payload[0] = 5U;
        accept_frame(&frame);
        CHECK(strstr(trace_capture_text(), "5001[2]=0500 1017[1]=01 9001[8]=3800200000000000 3001[1]=00"));
        CHECK((strstr(trace_capture_text(), "9001[8]=0000000000000000 3001[1]=01") != NULL) == include_zero_address);
        CHECK(strstr(trace_capture_text(), "9002[8]=7915000000000000\r\n"));
    }
    trace_capture_begin();
    tick = 10000U;
    accept_frame(&frame); /* 已完成后同一帧再次到达。 */
    CHECK(strstr(trace_capture_text(), "reject=noPending"));
    size_t reject_length = strlen(trace_capture_text());
    accept_frame(&frame); CHECK(strlen(trace_capture_text()) == reject_length);
    tick += 1000U; accept_frame(&frame); CHECK(strlen(trace_capture_text()) > reject_length);
    trace_capture_stop();
    message("PASS: B1 RK wire TLV log, optional frequency, heartbeat, timing, progress and 5/7-TLV result\n");
}
#endif

#if defined(TEST_B1_PROTOCOL_TRACE)
/* 接管前仍遵守最后时序的20ms间隔；SENT已领取且UART没有新发送也不能提前放行。 */
static void test_handoff_timing_gap(void)
{
    for (unsigned wrap = 0U; wrap < 2U; ++wrap) {
        reset_task_test();
        tick = wrap ? UINT32_MAX - 7U : 100U;
        rk_request_t setting = {0x25U, 3000U, 0U, 36U};
        rk_response_t event;
        CHECK(rk_link_request(&setting) == 0);
        process_request();
        deliver_result();
        CHECK(rk_link_receive(&event) == 1 && event.status == RK_RESPONSE_SENT);
        CHECK(!s_pending.delivery && requests.count == 0U && responses.count == 0U);
        CHECK(s_timing_gap && !rk_task_handoff_idle());
        tick += pdMS_TO_TICKS(RK_TIMING_GAP_MS) - 1U;
        CHECK(!rk_task_handoff_idle());
        ++tick;
        CHECK(rk_task_handoff_idle() && s_timing_gap);
        CHECK(tx_attempts == 1U); /* 没有靠再次process_request或追加UART发送清flag。 */
    }
    reset_task_test();
    message("PASS: SLT handoff retains final timing gap through exact boundary and tick wrap\n");
}

/* 直接调用真实RK任务内部函数：早到的EDK不能吞掉PC正在等待的时序发送事件。 */
static void test_handoff_unsent_timing(void)
{
    reset_task_test();
    rk_request_t setting = {0x25U, 3001U, 0U, 36U};
    rk_response_t event;
    CHECK(rk_link_request(&setting) == 0);
    process_request();
    CHECK(tx_attempts == 1U && s_pending.delivery && s_pending.response.status == RK_RESPONSE_SENT);
    setting.command_id = 0x26U; setting.cookie = 3002U;
    CHECK(rk_link_request(&setting) == 0);
    responses.count = 8U;
    reject_handoff_requests();
    CHECK(s_pending.delivery && s_pending.response.status == RK_RESPONSE_SENT);
    CHECK(requests.count == 1U && tx_attempts == 1U);
    responses.count = 0U;
    reject_handoff_requests();
    CHECK(requests.count == 0U && tx_attempts == 1U);
    CHECK(rk_link_receive(&event) == 1 && event.cookie == 3001U && event.status == RK_RESPONSE_SENT);
    CHECK(rk_link_receive(&event) == 1 && event.cookie == 3002U && event.status == RK_RESPONSE_TX_FAILED);
    CHECK(rk_link_receive(&event) == 0);

    /* 未发送设置的失败事件同样受背压保护，后续循环不重复发送或覆盖它。 */
    setting.cookie = 3003U;
    CHECK(rk_link_request(&setting) == 0);
    responses.count = 8U;
    reject_handoff_requests();
    CHECK(requests.count == 0U && s_pending.delivery && tx_attempts == 1U);
    CHECK(s_pending.response.cookie == 3003U && s_pending.response.status == RK_RESPONSE_TX_FAILED);
    reject_handoff_requests();
    CHECK(s_pending.delivery && tx_attempts == 1U);
    responses.count = 0U;
    reject_handoff_requests();
    CHECK(!s_pending.delivery);
    CHECK(rk_link_receive(&event) == 1 && event.cookie == 3003U && event.status == RK_RESPONSE_TX_FAILED);
    CHECK(rk_link_receive(&event) == 0);
    reset_task_test();
    message("PASS: early EDK preserves SENT and rejects unsent timings with response backpressure\n");
}
#endif

#if defined(TEST_RK_STOP)
/* 实际停止路径覆盖排队、FUNC等待、IDD采集/退出及结果队列背压；不能伪造完成事件。 */
static void test_stop_transaction(void)
{
    for (unsigned stage = 0U; stage < 6U; ++stage) {
        reset_task_test();
        handoff_test_reset();
        tlv_sm_t parser; tlv_sm_init(&parser);
        CHECK(tlv_sm_feed(&parser, 0x5AU) == 0);
        rk_request_t request = {stage >= 2U && stage <= 4U ? 21U : 17U, 765U, 0U, 0U};
        CHECK(rk_link_request(&request) == 0);
        if (stage) process_request();
        if (stage >= 3U && stage <= 4U) {
            complete_tx(100U);
            sample_at(105U, 105U, 1000U, MEMORY_MONITOR_VALID_ALL);
        }
        if (stage == 4U) { pump_samples(5105U); process_request(); }
        if (stage == 5U) {
            rk_response_t full = {.cookie = 765U, .status = RK_RESPONSE_COMPLETE};
            for (unsigned i = 0; i < 8U; ++i) CHECK(xQueueSend(rkResponseQueue, &full, 0U));
            tlv_frame_t result = task_result_frame(s_pending.sequence, request.command_id, 0U);
            accept_frame(&result);
            CHECK(s_pending.delivery);
        }
        unsigned transmissions = sent;
        rk_task_stop_begin();
        CHECK(process_stop(&parser) == 1 && !rk_task_stop_ready());
        process_request();
        CHECK(sent == transmissions);
        if (stage == 3U) {
            /* 停止后电源尚未OFF，新的采样也不得启动IDD退出发送。 */
            sample_at(106U, 106U, 9000U, MEMORY_MONITOR_VALID_ALL);
            CHECK(process_stop(&parser) == 1 && !rk_task_stop_ready() && sent == transmissions);
        }
        power_on = PWR_CTRL_STATE_OFF; handoff_test.reserved = 1;
        CHECK(process_stop(&parser) == 1 && !rk_task_stop_ready());
        handoff_test.reserved = 0; uart_idle = 0;
        CHECK(process_stop(&parser) == 1 && !rk_task_stop_ready());
        uart_idle = 1;
        CHECK(process_stop(&parser) == 1 && rk_task_stop_ready());
        CHECK(requests.count == 0U && responses.count == 0U && !s_pending.waiting && !s_pending.delivery);
        CHECK(sent == transmissions);
        tlv_frame_t decoded;
        CHECK(tlv_sm_get_frame(&parser, &decoded) < 0);
        uint8_t heartbeat[TLV_HEARTBEAT_FRAME_SIZE];
        CHECK(tlv_pack_heartbeat(123U, heartbeat, sizeof(heartbeat)) == sizeof(heartbeat));
        CHECK(feed(&parser, heartbeat, sizeof(heartbeat)) == 1U);
        rk_task_stop_end(); CHECK(process_stop(&parser) == 0);
        power_on = PWR_CTRL_STATE_ON;
        CHECK(rk_link_request(&request) == 0); process_request();
        CHECK(sent == transmissions + 1U && s_pending.waiting);
    }
    reset_task_test();
    message("PASS: RK protocol and transaction 0104 drops pending/results after OFF and UART recovery, restarts FUNC/IDD\n");
}
#endif

void test_main(void)
{
#if defined(TEST_RK_STOP)
    test_stop_transaction();
    finish(0U);
#endif
#if defined(TEST_B1_PROTOCOL_TRACE)
    test_handoff_timing_gap();
    test_handoff_unsent_timing();
#endif
    test_timing_and_frequency();
    test_rx_diagnostics(); test_rx_tlv_diagnostics();
    message("PASS: RK receive rejection diagnostics; matching rules unchanged\n");
    test_explicit_die_results(); test_reply_only_heartbeat();
    message("PASS: flag-directed die results, zero-address FAIL and reply-only RK heartbeat\n");
    test_dual_die_final_golden(); test_dual_die_final_rules();
    message("PASS: dual-die 3/5/7-TLV final decoder\n");
    test_empty_heartbeat();
    test_dual_tlv_progress(); test_progress_matching();
    test_command_table(); test_type_rules(); test_report_boundaries(); test_invalid_states();
    test_codec(); test_byte_timeout(); test_transaction(); test_unlimited_result_wait();
    test_timeout_recovery_backpressure();
    test_atomic_result(); test_latest_received_sequence(); test_result_stream();
    test_heartbeat(); test_ready_heartbeat(); test_task_fairness();
    test_idd_action_codec(); test_idd_transactions(); test_idd_failures(); test_register_address_codec();
#if defined(TEST_B1_PROTOCOL_TRACE)
    test_protocol_trace();
#endif
    message("PASS: FUNC/IDD categories, IDD enter/capture/exit transactions, standalone action and register address codecs\n");
    message("PASS: RK protocol and transaction, empty heartbeat, progress, atomic dual-die result, actual C offline only\n"); finish(0);
}
