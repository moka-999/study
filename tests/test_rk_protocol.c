/* 实际C离线故障注入：黄金帧、边界、连续流、四字段结果和单在途事务。 */
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
    CHECK(id == 0 && (length == 22U || length == 20U));
    tlv_frame_t frame;
    CHECK(tlv_unpack(buffer, length, &frame) == 1);
    CHECK((length == 22U && frame.units[0].type == TLV_TYPE_CMD) ||
          (length == 20U && frame.units[0].type == TLV_TYPE_HEARTBEAT));
    CHECK(tx_attempts < sizeof(tx_log) / sizeof(tx_log[0]));
    tx_log[tx_attempts].sequence = frame.sequence;
    tx_log[tx_attempts].tick = tick;
    tx_log[tx_attempts++].type = frame.units[0].type;
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
            CHECK(tlv_sm_get_frame(stream, &frame) > 0);
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

/* 六个已知类型的宽度规则必须同时约束打包和接收；未知扩展仍按显式长度保留。
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
        {TLV_TYPE_RESULT_TIME, 8U, 9U}
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
    rk_test_result_t result;
    CHECK(rk_result_decode(&decoded, &result) == 0 && result.command_id == 17U);

    /* 完整结果混入命令也不能交付；输出预填非零，检测是否泄漏上一次结果。 */
    CHECK(tlv_pack(&decoded, TLV_TYPE_CMD, value, 2U) == 0);
    memset(&result, 0xFF, sizeof(result));
    CHECK(rk_result_decode(&decoded, &result) == -1 && cleared(&result, sizeof(result)));
}

/* 查表下标损坏时必须先拒绝并复位，不能越界调用；下一帧仍可正常解析。 */
static void test_invalid_states(void)
{
    static const tlv_sm_state_t invalid_states[] = {TLV_SM_COUNT, (tlv_sm_state_t)-1};
    uint8_t wire[TLV_FRAME_MAX];
    unsigned length = tlv_pack_heartbeat(9U, wire, sizeof(wire));
    CHECK(length == 20U);
    for (unsigned i = 0U; i < sizeof(invalid_states) / sizeof(invalid_states[0]); ++i) {
        tlv_sm_t stream;
        tlv_sm_init(&stream);
        stream.state = invalid_states[i];
        stream.pos = 3U;
        stream.ready_len = 20U;
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
        rk_test_result_t decoded;
        CHECK(rk_result_decode(&frame, &decoded) == 0 && decoded.command_id == 17 && decoded.die == 2);
        CHECK(decoded.error_address == UINT64_C(0x123456789ABCDEF0) && decoded.elapsed_ms == UINT64_C(0x0123456789ABCDEF));
        --frame.count; CHECK(rk_result_decode(&frame, &decoded) == -1 && cleared(&decoded, sizeof(decoded)));
        frame = result; frame.units[0] = frame.units[1];
        memset(&decoded, 0xFF, sizeof(decoded));
        CHECK(rk_result_decode(&frame, &decoded) == -1 && cleared(&decoded, sizeof(decoded)));
    }
    tlv_sm_t stream; tlv_sm_init(&stream);
    CHECK(tlv_sm_get_frame(&stream, &frame) == -1);
    CHECK(feed(&stream, golden, sizeof(golden)) == 1);
    CHECK(feed(&stream, golden, sizeof(golden)) == 1);
    CHECK(feed(&stream, (uint8_t[]){0x5A,0x99}, 2) == 0);
    CHECK(feed(&stream, golden, sizeof(golden)) == 1);
    CHECK(feed(&stream, golden, 8) == 0); tick += 50;
    CHECK(feed(&stream, golden, sizeof(golden)) == 1);
    memcpy(wire, golden, sizeof(golden)); wire[19] ^= 1;
    CHECK(feed(&stream, wire, sizeof(golden)) == 0);
    CHECK(feed(&stream, golden, sizeof(golden)) == 1);
    CHECK(feed(&stream, golden, sizeof(golden) - 1) == 0);
    CHECK(feed(&stream, golden, sizeof(golden)) == 1);
    tick = UINT32_MAX - 20; CHECK(feed(&stream, golden, 8) == 0);
    tick = 31; CHECK(feed(&stream, golden, sizeof(golden)) == 1);
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
    tlv_frame_t result = result_frame(s_pending.sequence + 1U, 3, 0);
    accept_frame(&result); CHECK(s_pending.waiting);
    result = result_frame(s_pending.sequence, 4, 0); accept_frame(&result); CHECK(s_pending.waiting);
    result = result_frame(s_pending.sequence, 3, 0); --result.count;
    accept_frame(&result); CHECK(s_pending.waiting);
    result = result_frame(s_pending.sequence, 3, 0);
    responses.count = 8; accept_frame(&result); process_request();
    CHECK(s_pending.delivery && !s_pending.waiting && sent == 1);
    responses.count = 0; process_request(); CHECK(rk_link_receive(&response) == 1);
    CHECK(response.status == RK_RESPONSE_COMPLETE && response.cookie == 101 && !response.result.error_address);
    accept_frame(&result); process_request(); CHECK(rk_link_receive(&response) == 0);
    tx_failure = 1; CHECK(rk_link_request(&request) == 0); process_request(); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_TX_FAILED && sent == 1);
    tx_failure = 0; request.timeout_ms = 1000;
    CHECK(rk_link_request(&request) == 0); process_request(); CHECK(sent == 2);
    tick += 1000; process_request(); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_TIMEOUT);
    CHECK(rk_link_request(&request) == 0); tick += 10000; process_request(); CHECK(sent == 2);
    result = result_frame(s_pending.sequence, 3, 1); accept_frame(&result); process_request();
    CHECK(sent == 3 && rk_link_receive(&response) == 1);
    CHECK(response.status == RK_RESPONSE_RECOVERED && response.cookie == 101U);
    CHECK(cleared(&response.result, sizeof(response.result)) && rk_link_receive(&response) == 0);
    result = result_frame(s_pending.sequence, 3, 1); accept_frame(&result); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.result.error_address == 1);
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

/* 真实算法可运行十几分钟；0必须无限等待，与串口字节间50ms判定互不影响。 */
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
    tlv_frame_t result = result_frame(s_pending.sequence, 3U, UINT64_C(0x1234567890));
    accept_frame(&result); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_COMPLETE);
    CHECK(response.cookie == 501U && response.result.error_address == UINT64_C(0x1234567890));
    accept_frame(&result); process_request(); CHECK(rk_link_receive(&response) == 0);

    /* 无限等待跨tick回绕仍不误判超时，不重发；最终结果只交付一次。 */
    tick = UINT32_MAX - 5U;
    CHECK(rk_link_request(&request) == 0); process_request();
    tick = 10U; process_request();
    CHECK(sent == 2U && s_pending.waiting && !s_pending.timed_out);
    result = result_frame(s_pending.sequence, 3U, 0U);
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
        tlv_frame_t result = result_frame(sequence, 3U, 0U);
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
            result = result_frame(s_pending.sequence, 3U, 0U);
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
    tlv_frame_t result = result_frame(s_pending.sequence, 3U, 0U);
    accept_frame(&result); process_request();
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_TIMEOUT);
    CHECK(rk_link_receive(&response) == 1 && response.status == RK_RESPONSE_RECOVERED);
    accept_frame(&result); process_request(); CHECK(rk_link_receive(&response) == 0);
}

/* 毫秒桩核对发送边界、失败不忙重试、错过周期不追赶和tick回绕。
 * V2固定验收2000ms；独立B1复用本测试时仍按其自身构建周期检查。
 */
static void test_heartbeat(void)
{
    const TickType_t interval = pdMS_TO_TICKS(RK_HEARTBEAT_INTERVAL_MS);
#if !defined(B1_RK_UART)
    CHECK(interval == 2000U);
#endif
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
    CHECK(sent == 2U && memcmp(&pending, &s_pending, sizeof(pending)) == 0);
    CHECK(tx_log[1].type == TLV_TYPE_HEARTBEAT);
    CHECK(tx_log[1].sequence == tx_log[0].sequence + 1U);
    tlv_frame_t result = result_frame(s_pending.sequence, 3U, 0U);
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
void test_main(void)
{
    test_command_table(); test_type_rules(); test_invalid_states();
    test_codec(); test_transaction(); test_unlimited_result_wait();
    test_timeout_recovery_backpressure();
    test_heartbeat(); test_task_fairness();
    message("PASS: RK protocol and transaction, heartbeat scheduling, actual C offline only\n"); finish(0);
}
