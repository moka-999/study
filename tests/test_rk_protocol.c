/* 实际C离线故障注入：黄金帧、连续流、单在途事务及双颗粒最终包的独立解码。 */
#include "port.h"
#include "frame.h"
#include "state_machine.h"
#include "crc.h"
#include "rk_link.h"
#include <setjmp.h>
#include <string.h>
#include "rk_task_under_test.h"
#define S1(x) #x
#define S(x) S1(x)
#define CHECK(x) do { if (!(x)) fail("FAIL " S(__LINE__) ": " #x "\n"); } while (0)
static uint32_t tick;
static unsigned sent;
static int tx_failure, power_on = PWR_CTRL_STATE_ON;
/* 仅测试桩使用：记录发送尝试，包含未入队的失败帧；不修改生产任务的接口。 */
static unsigned tx_attempts;
static struct { uint64_t sequence; uint32_t tick; uint16_t type; } tx_log[16];
static jmp_buf task_exit;
static unsigned task_iterations, delay_calls, health_calls, rx_calls, rx_batch_calls;
static unsigned resume_power_after;
static int rx_mode;
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
void ota_app_progress(int task)
{
    CHECK(task == OTA_TASK_RK);
    ++health_calls;
}
void led_toggle(int led) { (void)led; }
int pwr_ctrl_get_state(void) { return power_on; }
int uart_poll_rx(int id, uint8_t *buffer, uint16_t length)
{
    CHECK(id == 0);
    if (rx_mode == 0) return 0;
    ++rx_calls;
    /* 配额失效时立即失败，不让真实无限任务卡死整个QEMU回归。 */
    CHECK(++rx_batch_calls <= 8U);
    if (rx_mode == 2) return UART_RX_GAP;
    memset(buffer, 0, length);
    return length;
}
int uart_send(int id, uint8_t *buffer, uint16_t length)
{
    CHECK(id == 0 && (length == 22U || length == 16U));
    tlv_frame_t frame;
    CHECK(tlv_unpack(buffer, length, &frame) == (length == 16U ? 0 : 1));
    CHECK((length == 22U && frame.units[0].type == TLV_TYPE_CMD) ||
          (length == 16U && frame.count == 0U));
    CHECK(tx_attempts < sizeof(tx_log) / sizeof(tx_log[0]));
    tx_log[tx_attempts].sequence = frame.sequence;
    tx_log[tx_attempts].tick = tick;
    tx_log[tx_attempts++].type = frame.count ? frame.units[0].type : TLV_TYPE_HEARTBEAT;
    if (tx_failure) return -1;
    ++sent;
    return 0;
}
typedef struct { unsigned count, size; uint8_t data[8][sizeof(rk_response_t)]; } fake_queue_t;
static fake_queue_t requests = {.size = sizeof(rk_request_t)};
static fake_queue_t responses = {.size = sizeof(rk_response_t)};
QueueHandle_t rkRequestQueue = &requests;
QueueHandle_t rkResponseQueue = &responses;
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
 * 同一条命令的两种查询应返回同一描述项；IDD 等未支持编号不能误入队。
 */
static void test_command_table(void)
{
    static const char *const names[] = {
        "gtest-stuck-address", "gtest-rand", "gtest-xor", "gtest-sub", "gtest-mul",
        "gtest-div", "gtest-or", "gtest-and", "gtest-seqinc", "gtest-solidbits",
        "gtest-checkerboard", "gtest-blockseq", "gtest-walkbits0", "gtest-walkbits1",
        "gtest-bitspread", "gtest-bitflip", "mats", "marchx", "marchc-", "marchc+"
    };
    static const uint16_t unsupported[] = {0U, 21U, 36U, UINT16_MAX};
    for (unsigned i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        const rk_command_descriptor_t *command = rk_command_find_by_id((uint16_t)(i + 1U));
        CHECK(command != NULL && command->command_id == i + 1U);
        CHECK(command->name != NULL && strcmp(command->name, names[i]) == 0);
        CHECK(rk_command_find_by_name(names[i]) == command);
    }
    for (unsigned i = 0U; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        CHECK(rk_command_find_by_id(unsupported[i]) == NULL);
    }
    CHECK(rk_command_find_by_name(NULL) == NULL);
    CHECK(rk_command_find_by_name("") == NULL);
    CHECK(rk_command_find_by_name("unknown-algorithm") == NULL);
    CHECK(rk_command_find_by_name("idd_0") == NULL);
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
        {TLV_TYPE_RESULT_FAIL, 1U, 2U}
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
    rk_request_t request = {3, 101, 5000};
    rk_response_t response;
    CHECK(rk_link_request(NULL) == -1);
    request.command_id = 21; CHECK(rk_link_request(&request) == -1); request.command_id = 3;
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
    memset(&s_pending, 0, sizeof(s_pending));
    requests.count = responses.count = 0U;
    tick = sent = tx_attempts = 0U;
    tx_failure = 0;
    power_on = PWR_CTRL_STATE_ON;
    task_iterations = delay_calls = health_calls = rx_calls = rx_batch_calls = 0U;
    resume_power_after = 0U;
    rx_mode = 0;
}

/* 进度必须匹配本次算法及Seq窗口，不改变最终结果缓存或测试生命周期。 */
static void test_progress_matching(void)
{
    reset_task_test();
    rk_request_t request = {17U, 706U, 0U};
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
            rk_request_t request = {17U, 701U, 0U};
            rk_response_t response;
            CHECK(rk_link_request(&request) == 0); process_request();
            uint64_t sequence = s_pending.sequence;
            tlv_frame_t old = result_frame(sequence, 17U, 0U);
            tlv_frame_t flag = scalar_frame(sequence, TLV_TYPE_RESULT_FAIL, mask != 0U);
            accept_frame(&old); accept_frame(&flag);
            CHECK(s_pending.waiting && rk_link_receive(&response) == 0);
            tlv_frame_t result = dual_die_frame(mask, reverse, 0U, UINT64_MAX, UINT64_MAX);
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
                CHECK(failure->error_address == (failure->die == 0U ? 0U : UINT64_MAX));
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
        rk_request_t request = {3U, 704U, 0U};
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
    rk_request_t request = {3U, 705U, 0U};
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
    rk_request_t request = {17U, 703U, 1000U};
    rk_response_t response;
    CHECK(rk_link_request(&request) == 0); process_request();
    uint64_t sequence = s_pending.sequence;
    tlv_frame_t frames[3];
    frame_init(&frames[0]); frames[0].sequence = sequence;
    frames[1] = progress_frame(sequence, 17U, 72U);
    frames[2] = dual_die_frame(3U, 1U, 0U, UINT64_MAX, 5645U);
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
    CHECK(response.result.failures[1].error_address == 0U && response.result.elapsed_ms == 5645U);
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
    rk_request_t request = {3U, 501U, 0U};
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
        rk_request_t request = {3U, 601U, 1000U};
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
    rk_request_t request = {3U, 603U, 1000U};
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
    rk_request_t request = {3U, 301U, 5000U};
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
 * 持续RX或GAP每轮都耗尽8块配额，仍须推进超时、心跳、OTA健康和调度让步。
 */
static void run_task_iterations(void)
{
    if (setjmp(task_exit) == 0) StartRkTask(NULL);
}

static void test_task_fairness(void)
{
    const unsigned interval = RK_HEARTBEAT_INTERVAL_MS;
    for (int mode = 1; mode <= 2; ++mode) {
        reset_task_test();
        rx_mode = mode;
        task_iterations = interval / 10U + 2U;
        rk_request_t request = {3U, 401U, 100U};
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
        rk_request_t request = {3U, 402U, 5000U};
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
/* 构造一个完整双颗粒包；mask仅为测试输入，不是线上字段。 */
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
    const uint64_t boundaries[] = {0U, UINT64_MAX};
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
    tlv_frame_t good = dual_die_frame(3U, 0U, 0U, UINT64_MAX, 0U), frame;
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

void test_main(void)
{
    test_dual_die_final_golden(); test_dual_die_final_rules();
    message("PASS: dual-die 3/5/7-TLV final decoder\n");
    test_empty_heartbeat();
    test_dual_tlv_progress(); test_progress_matching();
    test_command_table(); test_type_rules(); test_report_boundaries(); test_invalid_states();
    test_codec(); test_byte_timeout(); test_transaction(); test_unlimited_result_wait();
    test_timeout_recovery_backpressure();
    test_atomic_result(); test_latest_received_sequence(); test_result_stream();
    test_heartbeat(); test_ready_heartbeat(); test_task_fairness();
    message("PASS: RK protocol and transaction, empty heartbeat, progress, atomic dual-die result, actual C offline only\n"); finish(0);
}
