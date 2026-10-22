/* 离线执行正式B2代码与裸机主机编解码；仅替代队列、TCP侧业务端口和SPI硬件。 */
#include "b2_under_test.h"
#include "wire.h"
#include "crc.h"
#include <stdlib.h>

#define STR1(x) #x
#define STR(x) STR1(x)
#define CHECK(c) do { if (!(c)) fail("FAIL line " STR(__LINE__) ": " #c "\n"); } while (0)
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
    for (;;) {}
}
static _Noreturn void fail(const char *text) { message(text); finish(1U); }
void test_abort(void) { fail("CPU exception\n"); }
void test_assert(int truth) { CHECK(truth); }

static struct { unsigned size; int full; uint8_t bytes[80]; } queues[2];
static unsigned created, net_writes, mode_calls, published;
static int connected, test_busy, power_busy, ota_state, network_ok = 1, led_ok = 1;
static uint8_t slot = 1U;
static uint32_t now = 100U;
static uint8_t last_reply[B2_RESPONSE_SIZE];
static size_t last_length;
QueueHandle_t xQueueCreate(unsigned count, unsigned size)
{
    CHECK(count == 1U && created < 2U && size <= sizeof(queues[0].bytes));
    queues[created].size = size;
    return &queues[created++];
}
int xQueueSend(QueueHandle_t queue, const void *data, uint32_t ticks)
{
    (void)ticks;
    for (unsigned i = 0U; i < 2U; ++i) if (queue == &queues[i]) {
        CHECK(!queues[i].full);
        memcpy(queues[i].bytes, data, queues[i].size); queues[i].full = 1; return pdPASS;
    }
    fail("Bad queue\n");
}
int xQueueReceive(QueueHandle_t queue, void *data, uint32_t ticks)
{
    if (queue == &queues[1] && ticks == portMAX_DELAY && !queues[1].full)
        b2_service_poll(connected);
    for (unsigned i = 0U; i < 2U; ++i) if (queue == &queues[i]) {
        if (!queues[i].full) return pdFALSE;
        memcpy(data, queues[i].bytes, queues[i].size); queues[i].full = 0; return pdPASS;
    }
    fail("Bad queue\n");
}
void vTaskDelay(uint32_t ticks) { (void)ticks; fail("Unexpected task loop\n"); }
void vTaskDelete(void *task) { (void)task; fail("Unexpected task delete\n"); }
uint32_t HAL_GetTick(void) { return now; }
int pc_test_service_busy(void) { return test_busy; }
int pc_power_service_busy(void) { return power_busy; }
void ota_download_get_progress(ota_download_progress_t *p) { p->state = ota_state; }
uint8_t b2_board_site_id(void) { return slot; }
int network_config_set_tester(uint8_t tester, uint8_t site)
{
    CHECK(tester >= 1U && tester <= 21U && site >= 1U && site <= 8U);
    CHECK(!connected && !test_busy && !power_busy && !ota_state);
    ++net_writes; return network_ok;
}
int pc_test_round_update_modes(uint8_t mask, const uint8_t *modes)
{
    CHECK(mask <= 7U && modes);
    if (mask) ++mode_calls;
    return led_ok;
}
int b2_spi_start(void) { fail("Hardware forbidden\n"); }
int b2_spi_receive(b2_spi_packet_t *packet) { (void)packet; fail("Hardware forbidden\n"); }
int b2_spi_publish(uint32_t token, const uint8_t *frame, size_t size)
{
    CHECK(token == s_packet.token && size <= sizeof(last_reply));
    memcpy(last_reply, frame, size); last_length = size; ++published; return 1;
}

static uint8_t frame[B2_MAX_FRAME + 1U];
static size_t used;
static const uint8_t golden[] = {
    0x5A,0,0x15,0x90,0x1C,0,0,0,0,0,0,0,1,0x50,0,0,2,4,0,
    0x30,3,0,1,3,0xA0,5,0,6,0,1,1,0,2,0,0x27,0x40,0xA5
};
static const uint8_t golden_reply[] = {
    0x5A,0,0x15,0x90,0x1C,0,0,0,0,0,0,0,1,0x50,1,0,2,4,0,
    0x30,2,0,1,0,0x30,3,0,1,3,0x30,4,0,1,1,0xF2,2,0xA5
};
static void begin(uint16_t command)
{
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x5AU; frame[12] = 1U;
    used = 13U;
    frame[used++] = 0x50U; frame[used++] = 0U;
    frame[used++] = 0U; frame[used++] = 2U;
    frame[used++] = (uint8_t)command; frame[used++] = (uint8_t)(command >> 8);
}
static void field(uint16_t type, const uint8_t *value, size_t length)
{
    CHECK(used + 4U + length + 3U <= B2_MAX_FRAME);
    frame[used++] = (uint8_t)(type >> 8); frame[used++] = (uint8_t)type;
    frame[used++] = (uint8_t)(length >> 8); frame[used++] = (uint8_t)length;
    if (length) memcpy(frame + used, value, length);
    used += length;
}
static size_t seal(void)
{
    size_t payload = used - 13U;
    frame[1] = (uint8_t)(payload >> 8); frame[2] = (uint8_t)payload;
    uint16_t crc = crc16_modbus(frame, 3U);
    frame[3] = (uint8_t)crc; frame[4] = (uint8_t)(crc >> 8);
    crc = crc16_modbus(frame + 5U, used - 5U);
    frame[used] = (uint8_t)crc; frame[used + 1U] = (uint8_t)(crc >> 8);
    frame[used + 2U] = 0xA5U;
    return used + 3U;
}
static void expect_status(b2_status_t status)
{
    b2_request_t request;
    CHECK(b2_request_decode(frame, seal(), &request) && request.status == status);
}

static void parser_checks(void)
{
    CHECK(crc16_modbus((const uint8_t *)"123456789", 9U) == 0x4B37U);
    master_state_t state = {3U, 7U, {1U, 0U, 0U}};
    size_t size = master_build(1U, &state, frame, sizeof(frame));
    CHECK(size == sizeof(golden) && !memcmp(frame, golden, size));
    b2_request_t request;
    CHECK(b2_request_decode(frame, size, &request) && request.status == B2_STATUS_OK);
    CHECK(request.sequence == 1U && request.tester_id == 3U && request.thermal_mask == 7U);
    uint8_t response[B2_RESPONSE_SIZE];
    CHECK(b2_response_build(&request, B2_STATUS_OK, 3U, 1U, response, sizeof(response)) == sizeof(golden_reply));
    CHECK(!memcmp(response, golden_reply, sizeof(response)));
    master_reply_t reply;
    CHECK(master_decode(response, sizeof(response), 1U, &reply) && reply.site_id == 1U);
    CHECK(!master_decode(response, sizeof(response), 2U, &reply));
    CHECK(!b2_response_build(&request, B2_STATUS_OK, 3U, 0U, response, sizeof(response)));
    CHECK(!master_build(1U, &state, frame, 36U));
    for (size_t i = 0U; i < size; ++i) {
        memcpy(frame, golden, size); frame[i] ^= 1U;
        CHECK(!b2_request_decode(frame, size, &request));
    }
    const uint8_t bad_modes[][4] = {{0,1,0,0}, {0,1,3,0}, {0,1,2,2}};
    for (unsigned i = 0U; i < 3U; ++i) {
        begin(4U); field(0xA005U, bad_modes[i], 4U); expect_status(B2_STATUS_INVALID);
    }
    begin(4U); field(0xA005U, NULL, 0U); expect_status(B2_STATUS_INVALID);
    begin(4U); field(0xA005U, bad_modes[0], 1U); expect_status(B2_STATUS_INVALID);
    for (unsigned id = 0U; id <= 22U; ++id) {
        uint8_t value = (uint8_t)id;
        begin(4U); field(0x3003U, &value, 1U);
        expect_status(id && id <= 21U ? B2_STATUS_OK : B2_STATUS_INVALID);
    }
    uint8_t id = 3U;
    begin(4U); field(0x3003U, &id, 1U); field(0x3003U, &id, 1U); expect_status(B2_STATUS_INVALID);
    begin(4U); field(0x3004U, &id, 1U); expect_status(B2_STATUS_INVALID);
    for (unsigned cmd = 1U; cmd <= 3U; ++cmd) { begin((uint16_t)cmd); expect_status(B2_STATUS_UNSUPPORTED); }
    begin(4U); field(0x7000U, &id, 1U); expect_status(B2_STATUS_OK);
    begin(4U); field(0x5000U, &id, 1U); CHECK(!b2_request_decode(frame, seal(), &request));
    begin(4U); frame[13] = 0x70U; CHECK(!b2_request_decode(frame, seal(), &request));
    begin(4U); frame[18] = 1U; /* TLV Value仍是合法命令，确认高字节参与解析。 */
    expect_status(B2_STATUS_UNSUPPORTED);
    begin(4U); frame[used++] = 0U; CHECK(!b2_request_decode(frame, seal(), &request));
    begin(4U); frame[16] = 99U; CHECK(!b2_request_decode(frame, seal(), &request));
    begin(4U);
    for (unsigned i = 0U; i < 15U; ++i) field(0x7000U, NULL, 0U);
    expect_status(B2_STATUS_OK);
    field(0x7000U, NULL, 0U); CHECK(!b2_request_decode(frame, seal(), &request));
    static uint8_t padding[1014];
    begin(4U); field(0x7000U, padding, sizeof(padding));
    CHECK(seal() == B2_MAX_FRAME); expect_status(B2_STATUS_OK);
    b2_state_machine_t sm;
    b2_sm_begin(&sm);
    for (size_t i = 0U; i < B2_MAX_FRAME; ++i) b2_sm_feed(&sm, frame[i]);
    CHECK(b2_sm_end(&sm, &request));
    b2_sm_feed(&sm, 0xFFU); CHECK(!b2_sm_end(&sm, &request));
    for (size_t cut = 0U; cut < sizeof(golden); ++cut) {
        b2_sm_begin(&sm);
        for (size_t i = 0U; i < cut; ++i) b2_sm_feed(&sm, golden[i]);
        CHECK(!b2_sm_end(&sm, &request));
    }
    b2_sm_begin(&sm); b2_sm_feed(&sm, 0xFFU);
    for (size_t i = 0U; i < sizeof(golden); ++i) b2_sm_feed(&sm, golden[i]);
    CHECK(!b2_sm_end(&sm, &request)); /* 同一次CS不在垃圾后重同步。 */
}

static master_reply_t transact(uint64_t sequence, master_state_t state)
{
    s_packet.length = (uint16_t)master_build(sequence, &state, s_packet.data, sizeof(s_packet.data));
    CHECK(s_packet.length);
    ++s_packet.token;
    unsigned before = published;
    handle_packet();
    CHECK(published == before + 1U);
    master_reply_t reply;
    CHECK(master_decode(last_reply, last_length, sequence, &reply));
    return reply;
}
static void business_checks(void)
{
    CHECK(b2_service_prepare());
    master_state_t empty = {0}, full = {3U, 7U, {1U,0U,0U}};
    CHECK(transact(1U, empty).tester_id == 0U && !net_writes);
    CHECK(transact(2U, full).status == 0U && net_writes == 1U);
    uint8_t modes[3];
    CHECK(b2_service_site_id() == 1U && b2_service_thermal_modes(modes) == 7U && modes[0] == 1U);
    unsigned writes = mode_calls;
    CHECK(transact(2U, full).status == 0U && net_writes == 1U && mode_calls == writes);
    full.tester_id = 4U;
    CHECK(transact(2U, full).status == 1U && net_writes == 1U);
    connected = 1;
    CHECK(transact(3U, full).status == 2U && net_writes == 1U && mode_calls == writes);
    connected = 0;
    CHECK(transact(3U, full).status == 2U); /* 原请求重试保持原应答。 */
    CHECK(transact(4U, full).status == 0U && net_writes == 2U);
    full.tester_id = 5U;
    test_busy = 1; CHECK(transact(5U, full).status == 2U); test_busy = 0;
    power_busy = 1; CHECK(transact(6U, full).status == 2U); power_busy = 0;
    for (unsigned i = 1U; i <= 3U; ++i) {
        ota_state = (int)i; CHECK(transact(6U + i, full).status == 2U);
    }
    ota_state = 0;
    master_state_t partial = {0U, 2U, {0U,1U,0U}};
    connected = 1; test_busy = 1;
    CHECK(transact(10U, partial).status == 0U && net_writes == 2U);
    CHECK(b2_service_thermal_modes(modes) == 7U && modes[0] == 1U && modes[1] == 1U && !modes[2]);
    led_ok = 0; partial.thermal_mode[1] = 0;
    CHECK(transact(11U, partial).status == 4U);
    CHECK(b2_service_thermal_modes(modes) == 7U && modes[1] == 1U);
    led_ok = 1; connected = 0; test_busy = 0;
    network_ok = 0;
    CHECK(transact(12U, full).status == 4U);
    CHECK(b2_service_thermal_modes(modes) == 7U && modes[1] == 1U);
    network_ok = 1;
    slot = 8U; CHECK(transact(13U, empty).site_id == 8U && b2_service_site_id() == 8U);
    connected = 1; slot = 2U;
    CHECK(transact(14U, empty).status == 2U && b2_service_site_id() == 8U);
    connected = 0;
    CHECK(transact(15U, empty).site_id == 2U);
    now += 3000U;
    b2_link_diagnostics_t diag;
    b2_link_diagnostics(&diag); CHECK(!diag.online);
    CHECK(transact(16U, empty).status == 0U);
    b2_link_diagnostics(&diag); CHECK(diag.online && diag.duplicate_requests == 2U);
    CHECK(transact(UINT64_MAX, empty).status == 0U && transact(0U, empty).status == 0U);
    writes = net_writes;
    s_packet.data[0] = 0U; handle_packet();
    CHECK(net_writes == writes && s_diagnostics.invalid_frames == 1U);
}

void test_main(void)
{
    parser_checks();
    business_checks();
    message("PASS: B2 protocol / CS state machine / task retry / service atomicity / master codec\n");
    finish(0U);
}
