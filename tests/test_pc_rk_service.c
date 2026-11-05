/* QEMU执行正式PC分发、RK业务及命令表；平台队列/FRU状态由本文件控制。 */
#include "pc_test_service.h"
#include "pc_tester_info.h"
#include "monitor_task.h"
#include "result_led.h"
#include "rk_link.h"
#include "pc_power_service.h"
#include "b2_service.h"
#if defined(TEST_B1_CONSOLE_POWER)
#include "log_console.h"
static log_console_power_request_t console_request;
static unsigned console_completions;
static const char *console_error;
/* 串口字符与单槽交接另由实际log_console回归；这里仅替代请求/完成端口。 */
log_console_power_request_t log_console_take_power_request(void)
{
    log_console_power_request_t result = console_request;
    console_request = LOG_CONSOLE_POWER_NONE;
    return result;
}
void log_console_complete_power_request(const char *error)
{ ++console_completions; console_error = error; }
#endif
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
#include "v2_power_sim.h"
#endif
static uint32_t mock_tick = 1000U;
static pwr_ctrl_state_t mock_power_state = PWR_CTRL_STATE_OFF;
/* 仅PC_POWER_SEQUENCE_TEST打开这些平台端口；其余回归仍拒绝任何真实电源操作。 */
static int power_fixture, boot_failure;
static pwr_ctrl_result_t power_failure;
static unsigned power_on_calls, power_off_calls, boot_calls;
static boot_mode_t last_boot_mode;
static uint32_t boot_io_ms;
/* 仅故障回归注入同步设置返回值，正式电源编排及配置复用判定仍执行实际C代码。 */
static int config_result_override;
static monitor_boot_result_t config_boot_result;
static pwr_ctrl_result_t config_ddr_result;
static unsigned config_set_calls;
#if defined(B1_RK_UART)
static idd_stats_t measured_stats;
static monitor_snapshot_t measured_latest;
static int measurement_start_error;
static unsigned measurement_finish_calls;
/* 只替代采样任务的端口，不替代统计算法；下面显式注入的测量值不是硬件实测。 */
int monitor_idd_start(uint32_t duration, uint32_t minimum)
{
    if (measurement_start_error) return measurement_start_error;
    return idd_stats_begin(&measured_stats, mock_tick, duration, minimum);
}
void monitor_idd_cancel(void) { idd_stats_cancel(&measured_stats); }
void monitor_idd_finish(void) { ++measurement_finish_calls; idd_stats_finish(&measured_stats); }
int monitor_idd_get(idd_stats_t *out)
{
    if (!out) return 0;
    idd_stats_poll(&measured_stats, mock_tick);
    *out = measured_stats;
    return 1;
}
#endif
#include <stddef.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define S1(x) #x
#define S(x) S1(x)
#define CHECK(c) do { if (!(c)) fail("FAIL " S(__LINE__) ": " #c "\n"); } while (0)
#define START_JSON "{\"algorithmType\":\"functional\",\"algorithmName\":\"mats\",\"algorithmIndex\":37,\"power\":null,\"reboot\":false,\"bootMode\":null,\"TimingParameters\":{}}"
static int fru_error, queue_full, response_ready;
static unsigned queue_calls, fru_calls;
static result_led_state_t led_states[RESULT_LED_TARGET_COUNT];
static unsigned led_writes;
static int led_failure;
static uint8_t b2_modes[3], b2_known = 7U, b2_site;
uint8_t b2_service_thermal_modes(uint8_t modes[3])
{ memcpy(modes, b2_modes, sizeof(b2_modes)); return b2_known; }
uint8_t b2_service_site_id(void) { return b2_site; }
static char site_label[16] = "SITE-7";
static rk_request_t queued_request;
static rk_response_t queued_response;
QueueHandle_t rkRequestQueue = (void *)1;
QueueHandle_t rkResponseQueue = (void *)2;

/* 离线堆与退出使用既有Cortex-M7 QEMU启动器；不运行MCU外设初始化。 */
void *_sbrk(ptrdiff_t increment)
{
    static uint64_t heap[8192];
    static ptrdiff_t used;
    if (increment < -used || increment > (ptrdiff_t)sizeof(heap) - used) return (void *)-1;
    void *result = (uint8_t *)heap + used;
    used += increment;
    return result;
}
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
#if defined(B1_RK_UART)
size_t log_console_write(const char *text, size_t length)
{ CHECK(text && length < 144U); return length; }
#endif

int pc_tester_info_load(fru_info_t *out)
{
    ++fru_calls;
    if (fru_error) return -1;
    memset(out, 0, sizeof(*out));
    memcpy(out->site_label, site_label, sizeof(out->site_label));
    return 0;
}
int xQueueSend(QueueHandle_t queue, const void *data, uint32_t ticks)
{
    CHECK(queue == rkRequestQueue && ticks == 0U);
    ++queue_calls;
    if (queue_full) return pdFALSE;
    queued_request = *(const rk_request_t *)data;
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, void *data, uint32_t ticks)
{
    CHECK(queue == rkResponseQueue && ticks == 0U);
    if (!response_ready) return pdFALSE;
    *(rk_response_t *)data = queued_response;
    response_ready = 0;
    return pdTRUE;
}

/* 以下硬件入口若被调用则立即失败：外部已启动模式不许暗中改电源或Boot。 */
uint32_t HAL_GetTick(void) {
    if (power_fixture) return mock_tick;
#if defined(B1_RK_UART) || (defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION)
    return mock_tick;
#else
    fail("FAIL unexpected HAL_GetTick\n");
#endif
}
pwr_ctrl_state_t pwr_ctrl_get_state(void) {
    if (power_fixture) return mock_power_state;
#if defined(B1_RK_UART)
    return mock_power_state;
#else
    fail("FAIL unexpected power access\n");
#endif
}
pwr_ctrl_result_t monitor_request_power_on(uint32_t now)
{ (void)now; fail("FAIL power on\n"); }
uint8_t pwr_ctrl_is_configured(void) { return power_fixture != 0; }
pwr_ctrl_result_t monitor_request_training_power_on(ddr_memory_type_t type,
    const uint16_t mv[DDR_RAIL_COUNT], uint32_t now)
{
    CHECK(power_fixture && now == mock_tick && mock_power_state == PWR_CTRL_STATE_OFF);
    ++power_on_calls;
    if (power_failure != PWR_CTRL_OK) return power_failure;
    CHECK(ddr_power_configure_training(type, mv) == IF_OK);
    mock_power_state = PWR_CTRL_STATE_WAIT_FIRST_RAIL;
    return PWR_CTRL_OK;
}
pwr_ctrl_result_t monitor_request_power_off(uint32_t now)
{
    CHECK(power_fixture && now == mock_tick && mock_power_state == PWR_CTRL_STATE_ON);
    ++power_off_calls;
    if (power_failure != PWR_CTRL_OK) return power_failure;
    mock_power_state = PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF;
    return PWR_CTRL_OK;
}
monitor_boot_result_t monitor_set_boot_mode(boot_mode_t mode)
{
    last_boot_mode = mode;
    if (config_result_override) {
        CHECK(power_fixture && (unsigned)mode < BOOT_MODE_COUNT);
        ++config_set_calls;
        return config_boot_result;
    }
    CHECK(power_fixture && mock_power_state == PWR_CTRL_STATE_OFF && (unsigned)mode < BOOT_MODE_COUNT);
    ++boot_calls;
    mock_tick += boot_io_ms;
    return boot_failure ? MONITOR_BOOT_ERR_IO : MONITOR_BOOT_OK;
}
monitor_boot_result_t monitor_get_boot_mode(boot_mode_t *mode, uint16_t *mv)
{ (void)mode; (void)mv; fail("FAIL Boot query\n"); }
pwr_ctrl_result_t monitor_apply_ddr_config(ddr_memory_type_t type, const int16_t steps[3])
{
    if (config_result_override) {
        CHECK(power_fixture && (unsigned)type <= DDR_MEMORY_LPDDR4X && steps);
        ++config_set_calls;
#if defined(B1_RK_UART)
        /* 真实monitor的OFF缓存另有实际C回归；此夹具衔接分发与0501目标读取。 */
        if (config_ddr_result == PWR_CTRL_OK && mock_power_state == PWR_CTRL_STATE_OFF) {
            uint16_t targets[DDR_RAIL_COUNT];
            CHECK(ddr_power_build_step_targets(type, steps, targets) == IF_OK);
            CHECK(ddr_power_configure_training(type, targets) == IF_OK);
        }
#endif
        return config_ddr_result;
    }
    (void)type; (void)steps; fail("FAIL DDR write\n");
}
int monitor_get_latest(monitor_snapshot_t *out)
{
#if defined(B1_RK_UART)
    *out = measured_latest;
    return 1;
#elif defined(PC_POWER_SEQUENCE_TEST)
    /* V2真实编排夹具只提供无有效样本快照，验证SLT忙时仍可查询，不访问器件。 */
    CHECK(power_fixture && out);
    memset(out, 0, sizeof(*out));
    return 1;
#else
    (void)out; fail("FAIL monitor access\n");
#endif
}
HAL_StatusTypeDef result_led_set(result_led_target_t target, result_led_state_t state)
{ (void)target; (void)state; fail("FAIL LED write\n"); }
HAL_StatusTypeDef result_led_get_all(result_led_state_t states[3])
{
    if (led_failure) return HAL_ERROR;
    memcpy(states, led_states, sizeof(led_states));
    return HAL_OK;
}
HAL_StatusTypeDef result_led_set_all(const result_led_state_t states[RESULT_LED_TARGET_COUNT])
{
    if (led_failure) return HAL_ERROR;
    memcpy(led_states, states, sizeof(led_states));
    ++led_writes;
    return HAL_OK;
}

/* 核对完整18字节头及严格JSON；使用MCU本端序号，不接受请求序号99或RK序号。 */
static cJSON *decode(const pc_dispatch_response_t *response, uint16_t command, uint32_t sequence)
{
    const uint8_t *p = response->data;
    CHECK(response->length >= PC_PROTOCOL_HEADER_SIZE);
    uint32_t length = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    CHECK(length == response->length && p[4] == 0U && p[5] == 1U && p[6] == 0U && p[7] == 8U);
    CHECK(p[8] == 0U && p[9] == 0U && p[16] == 0U && p[17] == 0U);
    CHECK((((uint16_t)p[10] << 8) | p[11]) == command);
    CHECK((((uint32_t)p[12] << 24) | ((uint32_t)p[13] << 16) | ((uint32_t)p[14] << 8) | p[15]) == sequence);
    cJSON *body = NULL;
    CHECK(pc_json_parse_object(p + 18U, response->length - 18U, &body, NULL) == PC_JSON_OK);
    /* 所有响应共用此检查；模拟来源不作为PC JSON的额外字段输出。 */
    CHECK(cJSON_GetObjectItemCaseSensitive(body, "simulation") == NULL);
    return body;
}
static void string_is(const cJSON *body, const char *name, const char *expected)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(body, name);
    CHECK(cJSON_IsString(value) && strcmp(value->valuestring, expected) == 0);
}
static void number_is(const cJSON *body, const char *name, uint32_t expected)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(body, name);
    CHECK(cJSON_IsNumber(value) && value->valuedouble == (double)expected);
}
static void status_is(const pc_dispatch_response_t *response, const char *error, uint32_t sequence)
{
    cJSON *body = decode(response, PC_COMMAND_COMMAND_RESP, sequence);
    string_is(body, "status", error ? "failed" : "accepted");
    number_is(body, "algorithmIndex", 37U);
    number_is(body, "requestCommandId", PC_COMMAND_START_TEST);
    if (error) string_is(body, "errorCode", error);
    else CHECK(cJSON_GetObjectItemCaseSensitive(body, "errorCode") == NULL);
    CHECK(cJSON_GetObjectItemCaseSensitive(body, "testOnly") == NULL);
    CHECK(cJSON_GetObjectItemCaseSensitive(body, "testResults") == NULL);
    CHECK(cJSON_GetObjectItemCaseSensitive(body, "testTimeMs") == NULL);
    cJSON_Delete(body);
}
static int dispatch(uint16_t command, const char *json, uint32_t sequence, pc_dispatch_response_t *response)
{
    const pc_protocol_frame_t request = {.command_id = command, .service_id = 0U, .sequence = 99U,
        .json = (const uint8_t *)json, .json_length = (uint32_t)strlen(json)};
    return pc_dispatch_handle(&request, sequence, response);
}
static void start_expect(const char *json, const char *error)
{
    pc_dispatch_response_t response;
    /* 既有FUNC回归现在在显式整轮内运行；0106重复请求的busy不修改该轮。
     * 新增整轮回归使用dispatch直接调用，另行核对缺少0106必拒绝。
     */
    if (!pc_test_service_busy() && !pc_power_service_busy()) {
        CHECK(dispatch(PC_COMMAND_ROUND_BEGIN, "{}", 16U, &response) == PC_DISPATCH_RESPONSE_READY);
        cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 16U);
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(body, "status");
        CHECK(cJSON_IsString(status));
        CHECK(strcmp(status->valuestring, "completed") == 0 ||
              strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "errorCode")), "busy") == 0);
        cJSON_Delete(body);
    }
    unsigned before = queue_calls;
    CHECK(dispatch(PC_COMMAND_START_TEST, json, 17U, &response) == PC_DISPATCH_RESPONSE_READY);
    status_is(&response, error, 17U);
    if (error && strcmp(error, "busy") != 0) CHECK(queue_calls == before);
}
/* 反序构造两颗明细并用实际RK解码器归一化；零地址PASS必须在PC输出为null。 */
static void inject_mask(rk_response_status_t status, uint8_t mask, uint64_t first, uint64_t second)
{
    CHECK(!response_ready);
    memset(&queued_response, 0, sizeof(queued_response));
    queued_response.cookie = queued_request.cookie;
    queued_response.sequence = UINT64_C(0xAABBCCDDEEFF0011);
    queued_response.status = status;
    tlv_frame_t frame;
    frame_init(&frame);
    uint8_t command[] = {(uint8_t)queued_request.command_id, (uint8_t)(queued_request.command_id >> 8)};
    uint8_t failed = mask != 0U, elapsed[] = {0x10U, 0xA4U, 0U, 0U, 0U, 0U, 0U, 0U};
    CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_CMD, command, sizeof(command)) == 0);
    CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_FAIL, &failed, 1U) == 0);
    CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_TIME, elapsed, sizeof(elapsed)) == 0);
    for (unsigned n = 0U; n < 2U; ++n) {
        uint8_t die = 1U - n;
        uint64_t address = (mask & (1U << die)) ? (die ? second : first) : 0U;
        uint8_t value[8];
        for (unsigned i = 0U; i < 8U; ++i) value[i] = (uint8_t)(address >> (8U * i));
        CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_ADDR, value, sizeof(value)) == 0);
        CHECK(tlv_pack(&frame, TLV_TYPE_RESULT_DIE, &die, 1U) == 0);
    }
    CHECK(rk_final_result_decode(&frame, &queued_response.result) == 0);
    response_ready = 1;
}
static void inject(rk_response_status_t status, uint64_t address)
{
    inject_mask(status, address != 0U, address, 0U);
}
static void result_is(uint64_t address, const char *expected_address, const char *expected_site)
{
    pc_dispatch_response_t response;
    inject(RK_RESPONSE_COMPLETE, address);
    CHECK(pc_dispatch_next_response(0x12345678U, &response) == 1);
    cJSON *body = decode(&response, PC_COMMAND_TEST_RESULT_REPORT, 0x12345678U);
    string_is(body, "algorithmName", "mats");
    string_is(body, "algorithmType", "functional");
    number_is(body, "algorithmIndex", 37U);
    number_is(body, "testTimeMs", 42000U);
    const cJSON *results = cJSON_GetObjectItemCaseSensitive(body, "testResults");
    CHECK(cJSON_IsArray(results) && cJSON_GetArraySize(results) == 2);
    const cJSON *chip = cJSON_GetArrayItem(results, 0);
    /* JSON仍按键名解释；这里只核对本端输出顺序与协议示例一致，兼顾PASS和FAIL。 */
    static const char *const result_fields[] = {
        "mirrorIndex", "testStatus", "resultDesc", "physicalAddr"
    };
    const cJSON *field = chip->child;
    for (size_t i = 0U; i < sizeof(result_fields) / sizeof(result_fields[0]); ++i) {
        CHECK(field && field->string && strcmp(field->string, result_fields[i]) == 0);
        field = field->next;
    }
    CHECK(field == NULL);
    string_is(body, "siteId", expected_site);
    CHECK(!cJSON_GetObjectItemCaseSensitive(chip, "siteId") && !cJSON_GetObjectItemCaseSensitive(chip, "measurementData"));
    string_is(chip, "physicalAddr", "");
    number_is(chip, "mirrorIndex", 0U);
    number_is(cJSON_GetArrayItem(results, 1), "mirrorIndex", 1U);
    number_is(cJSON_GetArrayItem(results, 1), "testStatus", 1U);
    number_is(chip, "testStatus", address == 0U ? 1U : 0U);
    const cJSON *desc = cJSON_GetObjectItemCaseSensitive(chip, "resultDesc");
    CHECK(cJSON_IsObject(desc));
    if (expected_address) string_is(desc, "firstErrorAddress", expected_address);
    else CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(desc, "firstErrorAddress")));
    string_is(desc, "errorType", "");
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
    const cJSON *measurement = cJSON_GetObjectItemCaseSensitive(body, "measurementData");
    CHECK(cJSON_IsObject(measurement) && cJSON_GetArraySize(measurement) == 18);
#elif defined(B1_RK_UART)
    const cJSON *measurement = cJSON_GetObjectItemCaseSensitive(body, "measurementData");
    if (measured_stats.voltage[0].valid_count || measured_stats.current[0].valid_count)
        CHECK(cJSON_IsObject(measurement) && cJSON_GetArraySize(measurement) == 18);
    else CHECK(cJSON_IsNull(measurement));
#else
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(body, "measurementData")));
#endif
    cJSON_Delete(body);
    CHECK(!pc_test_service_busy());
    CHECK(pc_dispatch_next_response(19U, &response) == 0);
}
static void *no_memory(size_t bytes) { (void)bytes; return NULL; }

/* 逐次注入组包分配失败，覆盖根对象、颗粒明细和测量字段的每个分配点。 */
static unsigned delivery_allocations, delivery_fail_at;
#if defined(B1_RK_UART)
static void b1_sample(uint8_t valid, int32_t mv, uint32_t ma);
#endif
static void *delivery_allocator(size_t bytes)
{
    unsigned position = delivery_allocations++;
    return position == delivery_fail_at ? NULL : malloc(bytes);
}

static void delivery_retry_check(void)
{
    pc_dispatch_response_t response;
    start_expect(START_JSON, NULL);
#if defined(B1_RK_UART)
    b1_sample(3U, 1800, 300U);
#endif
    inject_mask(RK_RESPONSE_COMPLETE, 3U, 0x1234U, 0xABCDU);
    delivery_allocations = 0U;
    delivery_fail_at = UINT32_MAX;
    pc_json_configure_allocator(delivery_allocator, free);
    CHECK(pc_dispatch_next_response(30U, &response) == PC_DISPATCH_RESPONSE_READY);
    unsigned allocation_count = delivery_allocations;
    pc_json_configure_allocator(NULL, NULL);
    CHECK(allocation_count > 0U && !pc_test_service_busy());

    for (unsigned failure = 0U; failure < allocation_count; ++failure) {
        start_expect(START_JSON, NULL);
#if defined(B1_RK_UART)
        b1_sample(3U, 1800, 300U);
        unsigned finishes = measurement_finish_calls;
#endif
        unsigned submitted = queue_calls;
        inject_mask(RK_RESPONSE_COMPLETE, 3U, 0x1234U, 0xABCDU);
        delivery_allocations = 0U;
        delivery_fail_at = failure;
        pc_json_configure_allocator(delivery_allocator, free);
        CHECK(pc_dispatch_next_response(31U, &response) == 0);
        CHECK(pc_test_service_busy() && !response_ready && queue_calls == submitted);
        pc_json_configure_allocator(no_memory, free);
        CHECK(pc_dispatch_next_response(32U, &response) == 0);
        CHECK(pc_test_service_busy() && queue_calls == submitted);
#if defined(B1_RK_UART)
        CHECK(measurement_finish_calls == finishes + 1U);
#endif
        pc_json_configure_allocator(NULL, NULL);
        CHECK(pc_dispatch_next_response(33U, &response) == PC_DISPATCH_RESPONSE_READY);
        cJSON *body = decode(&response, PC_COMMAND_TEST_RESULT_REPORT, 33U);
        number_is(body, "algorithmIndex", 37U);
        number_is(body, "testTimeMs", 42000U);
#if defined(B1_RK_UART)
        CHECK(measurement_finish_calls == finishes + 1U);
        number_is(cJSON_GetObjectItemCaseSensitive(body, "measurementData"), "Idd1Ave", 300U);
#endif
        const cJSON *chips = cJSON_GetObjectItemCaseSensitive(body, "testResults");
        const cJSON *chip = cJSON_GetArrayItem(chips, 1);
        number_is(chip, "testStatus", 0U);
        string_is(cJSON_GetObjectItemCaseSensitive(chip, "resultDesc"),
                  "firstErrorAddress", "0x000000000000ABCD");
        cJSON_Delete(body);
        CHECK(!pc_test_service_busy() && queue_calls == submitted);
        CHECK(pc_dispatch_next_response(34U, &response) == 0);
    }

    /* 待组包结果仍受连接归属和0107废弃规则限制，不跨连接补报。 */
    for (unsigned disconnect = 0U; disconnect < 2U; ++disconnect) {
        start_expect(START_JSON, NULL);
        inject(RK_RESPONSE_COMPLETE, 0U);
        pc_json_configure_allocator(no_memory, free);
        CHECK(pc_dispatch_next_response(35U, &response) == 0 && pc_test_service_busy());
        pc_json_configure_allocator(NULL, NULL);
        if (disconnect) pc_dispatch_disconnected();
        else CHECK(pc_test_round_end(0U, 36U, &response) == PC_DISPATCH_RESPONSE_READY);
        CHECK(pc_dispatch_next_response(37U, &response) == 0 && !pc_test_service_busy());
    }

    /* TIMEOUT组包失败时不能先消费后到的RECOVERED，避免错误地漏报超时。 */
    start_expect(START_JSON, NULL);
    inject(RK_RESPONSE_TIMEOUT, 0U);
    pc_json_configure_allocator(no_memory, free);
    CHECK(pc_dispatch_next_response(38U, &response) == 0 && pc_test_service_busy());
    inject(RK_RESPONSE_RECOVERED, 0U);
    CHECK(pc_dispatch_next_response(39U, &response) == 0 && response_ready);
    pc_json_configure_allocator(NULL, NULL);
    CHECK(pc_dispatch_next_response(40U, &response) == PC_DISPATCH_RESPONSE_READY);
    status_is(&response, "timeout", 40U);
    CHECK(pc_test_service_busy() && response_ready);
    CHECK(pc_dispatch_next_response(41U, &response) == 0 && !pc_test_service_busy());
    message("PASS: result allocation retry, single delivery, disconnect/discard and timeout ordering\n");
}

#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
#define POWER_START "{\"algorithmType\":\"functional\",\"algorithmName\":\"mats\",\"algorithmIndex\":37,\"power\":{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1800\",\"vdd2\":\"1100\",\"vddq\":\"600\"},\"reboot\":false,\"bootMode\":\"emmc\",\"TimingParameters\":{}}"
#define CHANGED_START "{\"algorithmType\":\"functional\",\"algorithmName\":\"mats\",\"algorithmIndex\":37,\"power\":{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1810\",\"vdd2\":\"1100\",\"vddq\":\"600\"},\"reboot\":false,\"bootMode\":\"emmc\",\"TimingParameters\":{}}"
static cJSON *query(uint16_t command, uint16_t reply)
{
    pc_dispatch_response_t response;
    CHECK(dispatch(command, "{}", 41U, &response) == PC_DISPATCH_RESPONSE_READY);
    return decode(&response, reply, 41U);
}
static void control(uint16_t command, const char *json, const char *status, const char *error)
{
    pc_dispatch_response_t response;
    CHECK(dispatch(command, json, 41U, &response) == PC_DISPATCH_RESPONSE_READY);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 41U);
    string_is(body, "status", status);
    if (error) string_is(body, "errorCode", error);
    cJSON_Delete(body);
}
static void power_simulation_check(void)
{
    pc_dispatch_response_t response;
    /* 前面的同一套在途/断线/超时回归也运行在模拟配置下。此处补测全部电源入口。 */
    control(PC_COMMAND_POWER_OFF, "{}", "completed", NULL);
    control(PC_COMMAND_RESET_RK, "{}", "failed", "invalidState");
    control(PC_COMMAND_QUERY_BOOT_MODE, "{}", "failed", "notConfigured");
    cJSON *body = query(PC_COMMAND_QUERY_POWER_MONITOR, PC_COMMAND_POWER_MONITOR_RESP);
    number_is(body, "validMask", 0U);
    CHECK(!cJSON_GetObjectItemCaseSensitive(body, "currentMa"));
    cJSON_Delete(body);
    const char *boot_json[] = {"{\"bootMode\":\"usbMaskrom\"}", "{\"bootMode\":\"sdCard\"}",
        "{\"bootMode\":\"emmc\"}", "{\"bootMode\":\"fspiM0\"}", "{\"bootMode\":\"fspiM1\"}",
        "{\"bootMode\":\"fspiM2\"}", "{\"bootMode\":\"emmcSd\"}"};
    for (unsigned mode = 0; mode < BOOT_MODE_COUNT; ++mode) {
        uint16_t target;
        CHECK(mcp4725_boot_mode_target_mv((boot_mode_t)mode, &target) == IF_OK);
        control(PC_COMMAND_SET_BOOT_MODE, boot_json[mode], "completed", NULL);
        body = query(PC_COMMAND_QUERY_BOOT_MODE, PC_COMMAND_BOOT_MODE_RESP);
        number_is(body, "targetMv", target); cJSON_Delete(body);
    }
    control(PC_COMMAND_SET_DDR_CONFIG, "{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1810\",\"vdd2\":\"1100\",\"vddq\":\"600\"}", "failed", "invalidState");
    control(PC_COMMAND_SET_DDR_CONFIG, "{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1800\",\"vdd2\":\"1100\",\"vddq\":\"600\"}", "completed", NULL);
    control(PC_COMMAND_POWER_ON, "{}", "accepted", NULL);
    control(PC_COMMAND_SET_BOOT_MODE, boot_json[0], "failed", "invalidState");
    control(PC_COMMAND_RESET_RK, "{}", "completed", NULL);
    control(PC_COMMAND_SET_DDR_CONFIG, "{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1810\",\"vdd2\":\"1095\",\"vddq\":\"610\"}", "completed", NULL);
    /* 0509经正式分发后，0602快照围绕字符串mV指定的目标，不取整或解释成档位。 */
    body = query(PC_COMMAND_QUERY_POWER_MONITOR, PC_COMMAND_POWER_MONITOR_RESP);
    const cJSON *voltage = cJSON_GetObjectItemCaseSensitive(body, "voltageMv");
    int delta = cJSON_GetArrayItem(voltage, 0)->valueint - 1810;
    CHECK(delta == -10 || delta == 0 || delta == 10);
    CHECK(cJSON_GetArrayItem(voltage, 1)->valueint == 1095 + delta);
    CHECK(cJSON_GetArrayItem(voltage, 2)->valueint == 610 + delta);
    cJSON_Delete(body);
    const char *const invalid_power[] = {
        "{\"memoryType\":\"LPDDR4X\",\"vdd1\":1800,\"vdd2\":\"1100\",\"vddq\":\"600\"}",
        "{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1801\",\"vdd2\":\"1100\",\"vddq\":\"600\"}",
        "{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1800\",\"vdd2\":\"1101\",\"vddq\":\"600\"}",
        "{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1800\",\"vdd2\":\"1100\",\"vddq\":\"601\"}",
        "{\"memoryType\":\"LPDDR4X\",\"vdd1Steps\":0,\"vdd2Steps\":0,\"vddqSteps\":0}",
        "{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1800\",\"vdd1\":\"1810\",\"vddq\":\"600\"}",
        "{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1800mV\",\"vdd2\":\"1100\",\"vddq\":\"600\"}",
    };
    for (unsigned i = 0U; i < sizeof(invalid_power) / sizeof(invalid_power[0]); ++i)
        control(PC_COMMAND_SET_DDR_CONFIG, invalid_power[i], "failed", "invalidArgument");
    body = query(PC_COMMAND_QUERY_POWER_MONITOR, PC_COMMAND_POWER_MONITOR_RESP);
    voltage = cJSON_GetObjectItemCaseSensitive(body, "voltageMv");
    CHECK(cJSON_GetArrayItem(voltage, 0)->valueint == 1810 + delta);
    CHECK(cJSON_GetArrayItem(voltage, 1)->valueint == 1095 + delta);
    CHECK(cJSON_GetArrayItem(voltage, 2)->valueint == 610 + delta);
    cJSON_Delete(body);
    control(PC_COMMAND_SET_DDR_CONFIG, "{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"11790\",\"vdd2\":\"1100\",\"vddq\":\"600\"}", "failed", "invalidArgument");
    control(PC_COMMAND_FIRMWARE_UPGRADE_REQUEST, "{\"action\":\"mcu_begin\"}", "failed", "notSupported");

    uint32_t cycles = v2_power_sim_cycles();
    start_expect(POWER_START, NULL);
    CHECK(v2_power_sim_cycles() == cycles + 1U && queued_request.command_id == 17U);
    /* 轮询不产生样本，不返回busy；即使PC读取100次，统计仍只有最初那次。 */
    for (unsigned i = 0; i < 100U; ++i) {
        body = query(PC_COMMAND_QUERY_POWER_MONITOR, PC_COMMAND_POWER_MONITOR_RESP);
        const cJSON *stats = cJSON_GetObjectItemCaseSensitive(body, "statistics");
        string_is(stats, "state", "running");
        const cJSON *values = cJSON_GetObjectItemCaseSensitive(stats, "currentAvgMa");
        CHECK(cJSON_GetArrayItem(values, 0)->valuedouble == 270.0);
        cJSON_Delete(body);
        CHECK(pc_dispatch_next_response(42U, &response) == 0 && pc_test_service_busy());
    }
    body = query(PC_COMMAND_QUERY_POWER_STATUS, PC_COMMAND_POWER_STATUS_RESP);
    number_is(body, "powerCycleCount", cycles + 1U); cJSON_Delete(body);
    body = query(PC_COMMAND_QUERY_BOOT_MODE, PC_COMMAND_BOOT_MODE_RESP);
    string_is(body, "bootMode", "emmc"); cJSON_Delete(body);
    for (unsigned i = 0; i < 3U; ++i) { mock_tick += 500U; pc_dispatch_poll(); }
    body = query(PC_COMMAND_QUERY_POWER_MONITOR, PC_COMMAND_POWER_MONITOR_RESP);
    char *snapshot = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(body, "statistics"));
    CHECK(snapshot); cJSON_Delete(body);
    /* 直到注入真实RK业务队列的完成事件才有0105；18项与0602来自同一统计。 */
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(42U, &response) == 1);
    body = decode(&response, PC_COMMAND_TEST_RESULT_REPORT, 42U);
    number_is(body, "testTimeMs", 42000U);
    const cJSON *chip = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(body, "testResults"), 0);
    const cJSON *measurement = cJSON_GetObjectItemCaseSensitive(body, "measurementData");
    number_is(chip, "testStatus", 1U);
    number_is(measurement, "Vdd1Ave", 1800U); number_is(measurement, "Idd1Ave", 300U);
    number_is(measurement, "Idd1Peak", 330U); number_is(measurement, "Idd1Min", 270U);
    cJSON *expected = cJSON_Parse(snapshot); cJSON_free(snapshot); CHECK(expected);
    const char *arrays[] = {"voltageAvgMv", "voltagePeakMv", "voltageMinMv", "currentAvgMa", "currentPeakMa", "currentMinMa"};
    const char *fields[] = {"Vdd1Ave", "Vdd1Peak", "Vdd1Min", "Vdd2Ave", "Vdd2Peak", "Vdd2Min", "VddqAve", "VddqPeak", "VddqMin",
        "Idd1Ave", "Idd1Peak", "Idd1Min", "Idd2Ave", "Idd2Peak", "Idd2Min", "IddqAve", "IddqPeak", "IddqMin"};
    for (unsigned i = 0; i < 18U; ++i) {
        const cJSON *values = cJSON_GetObjectItemCaseSensitive(expected, arrays[(i / 9U) * 3U + i % 3U]);
        number_is(measurement, fields[i], (uint32_t)cJSON_GetArrayItem(values, (i % 9U) / 3U)->valuedouble);
    }
    cJSON_Delete(body); cJSON_Delete(expected);
    mock_tick += 10000U; pc_dispatch_poll();
    body = query(PC_COMMAND_QUERY_POWER_MONITOR, PC_COMMAND_POWER_MONITOR_RESP);
    const cJSON *stats = cJSON_GetObjectItemCaseSensitive(body, "statistics");
    string_is(stats, "state", "complete");
    CHECK(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(stats, "currentAvgMa"), 0)->valuedouble == 300.0);
    cJSON_Delete(body);

    cycles = v2_power_sim_cycles();
    /* 开机时改Boot或切换内存类型均拒绝，原模拟训练配置仍须可复用。 */
    control(PC_COMMAND_SET_BOOT_MODE, "{\"bootMode\":\"emmc\"}", "failed", "invalidState");
    control(PC_COMMAND_SET_DDR_CONFIG,
        "{\"memoryType\":\"LPDDR4\",\"vdd1\":\"1800\",\"vdd2\":\"1100\",\"vddq\":\"1100\"}",
        "failed", "invalidState");
    start_expect(POWER_START, NULL); /* 相同训练配置且reboot=false不增加虚拟冷重启。 */
    CHECK(v2_power_sim_cycles() == cycles);
    result_is(0U, NULL, "");
    start_expect(CHANGED_START, NULL); /* 变更目标则MCU强制虚拟冷重启。 */
    CHECK(v2_power_sim_cycles() == cycles + 1U);
    body = query(PC_COMMAND_QUERY_POWER_MONITOR, PC_COMMAND_POWER_MONITOR_RESP);
    stats = cJSON_GetObjectItemCaseSensitive(body, "statistics");
    CHECK(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(stats, "voltageAvgMv"), 0)->valuedouble == 1800.0);
    cJSON_Delete(body);
    result_is(1U, "0x0000000000000001", "");
    /* 最长FRU控制字符全部转义后，含统计结果仍装得进响应缓冲。 */
    memset(site_label, 1, sizeof(site_label) - 1U); site_label[15] = 0;
    start_expect(CHANGED_START, NULL);
    result_is(UINT64_MAX, "0xFFFFFFFFFFFFFFFF", site_label);
    /* 模拟8小时经过uint32时基回绕，PC不查询也持续积累；均值不依赖查询次数。 */
    mock_tick = UINT32_MAX - 1000U;
    start_expect(CHANGED_START, NULL);
    for (unsigned i = 0; i < 57600U; ++i) { mock_tick += 500U; pc_dispatch_poll(); }
    body = query(PC_COMMAND_QUERY_POWER_MONITOR, PC_COMMAND_POWER_MONITOR_RESP);
    stats = cJSON_GetObjectItemCaseSensitive(body, "statistics");
    string_is(stats, "state", "running");
    CHECK(fabs(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(stats, "currentAvgMa"), 0)->valuedouble -
               (300.0 - 30.0 / 57601.0)) < 1e-9);
    cJSON_Delete(body);
    result_is(0U, NULL, site_label);
    message("PASS: PC RK service power simulation: polling while busy, 18 metrics, freeze/reset, real RK queue, no hardware calls\n");
}
#endif

#if defined(B1_RK_UART)
static cJSON *b1_query(void)
{
    pc_dispatch_response_t response;
    CHECK(dispatch(PC_COMMAND_QUERY_POWER_MONITOR, "{}", 42U, &response) == PC_DISPATCH_RESPONSE_READY);
    return decode(&response, PC_COMMAND_POWER_MONITOR_RESP, 42U);
}

static void b1_sample(uint8_t valid, int32_t mv, uint32_t ma)
{
    mock_tick += 2000U;
    memset(&measured_latest, 0, sizeof(measured_latest));
    measured_latest.timestamp_ms = mock_tick;
    measured_latest.power_state = PWR_CTRL_STATE_ON;
    measured_latest.memory.valid_mask = valid;
    for (unsigned rail = 0; rail < 3U; ++rail) {
        measured_latest.memory.voltage_mv[rail] = mv;
        measured_latest.memory.idd[rail].current_ma = ma;
        measured_latest.memory.idd[rail].current_ua = (uint64_t)ma * 1000U;
    }
    CHECK(idd_stats_feed(&measured_stats, mock_tick, mock_tick, &measured_latest.memory) == 1);
}

static void b1_measurement_check(void)
{
    pc_dispatch_response_t response;
    start_expect(START_JSON, NULL);
    CHECK(measured_stats.attempts == 0U);
    b1_sample(3U, 1790, 270U);
    b1_sample(3U, 1810, 330U);
    /* PC连续查询仅复制同一统计，不产生新样本，也不因在途而返回busy。 */
    for (unsigned i = 0; i < 100U; ++i) {
        cJSON *body = b1_query();
        const cJSON *stats = cJSON_GetObjectItemCaseSensitive(body, "statistics");
        string_is(stats, "state", "running");
        const cJSON *values = cJSON_GetObjectItemCaseSensitive(stats, "currentAvgMa");
        CHECK(cJSON_GetArrayItem(values, 0)->valuedouble == 300.0);
        CHECK(measured_stats.attempts == 2U && pc_test_service_busy());
        cJSON_Delete(body);
    }
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(43U, &response) == 1);
    cJSON *body = decode(&response, PC_COMMAND_TEST_RESULT_REPORT, 43U);
    cJSON *data = cJSON_GetObjectItemCaseSensitive(body, "measurementData");
    number_is(data, "Vdd1Ave", 1800U); number_is(data, "Vdd1Peak", 1810U); number_is(data, "Vdd1Min", 1790U);
    number_is(data, "Idd1Ave", 300U); number_is(data, "Idd1Peak", 330U); number_is(data, "Idd1Min", 270U);
    CHECK(cJSON_GetArraySize(data) == 18 && measured_stats.state == IDD_STATS_COMPLETE);
    cJSON_Delete(body);
    mock_tick += 10000U;
    body = b1_query();
    string_is(cJSON_GetObjectItemCaseSensitive(body, "statistics"), "state", "complete");
    CHECK(measured_stats.attempts == 2U);
    cJSON_Delete(body);

    /* 新测试清空旧均值；ADC无效不填0，PAC有效数据独立保留，RK PASS不受其替代。 */
    start_expect(START_JSON, NULL);
    b1_sample(MEMORY_MONITOR_VALID_IDD, 0, 123U);
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(44U, &response) == 1);
    body = decode(&response, PC_COMMAND_TEST_RESULT_REPORT, 44U);
    cJSON *chip = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(body, "testResults"), 0);
    data = cJSON_GetObjectItemCaseSensitive(body, "measurementData");
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(data, "Vdd1Ave")));
    number_is(data, "Idd1Ave", 123U); number_is(chip, "testStatus", 1U);
    CHECK(measured_stats.state == IDD_STATS_TIMEOUT);
    cJSON_Delete(body);

    /* 最大整数、最长转义标签和64位错误/耗时仍必须装入完整响应，不能断连。 */
    memset(site_label, 1, sizeof(site_label) - 1U); site_label[15] = 0;
    start_expect(START_JSON, NULL);
    b1_sample(3U, INT32_MIN, UINT32_MAX / 1000U);
    result_is(UINT64_MAX, "0xFFFFFFFFFFFFFFFF", site_label);
    memcpy(site_label, "SITE-7", 7U);
    message("PASS: PC RK service B1 real-path guards, 100 queries, 18 metrics, partial samples and freeze/reset\n");
}
#endif

#if defined(ENABLE_V2_MATS_FAIL_TEST) && ENABLE_V2_MATS_FAIL_TEST
/* 故障注入只改变已完成MATS的PC视图；验证其它19项、真实结果和状态门禁不变。 */
static void mats_fail_scope_check(void)
{
    pc_dispatch_response_t response;
    for (uint16_t id = 1U; id <= 20U; ++id) {
        const rk_command_descriptor_t *algorithm = rk_command_find_by_id(id);
        CHECK(algorithm != NULL);
        cJSON *request = cJSON_Parse(START_JSON);
        CHECK(request && cJSON_ReplaceItemInObjectCaseSensitive(request, "algorithmName",
                                                              cJSON_CreateString(algorithm->name)));
        char *json = cJSON_PrintUnformatted(request);
        CHECK(json != NULL);
        for (unsigned sample = 0U; sample < 4U; ++sample) {
            start_expect(json, NULL);
            CHECK(queued_request.command_id == id);
            CHECK(pc_dispatch_next_response(18U, &response) == 0 && pc_test_service_busy());
            start_expect(START_JSON, "busy");
            cJSON *body = query(PC_COMMAND_QUERY_POWER_MONITOR, PC_COMMAND_POWER_MONITOR_RESP);
            CHECK(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(body, "statistics")));
            cJSON_Delete(body);
            inject_mask(RK_RESPONSE_COMPLETE, sample, 1U, UINT64_MAX);
            rk_final_result_t original = queued_response.result;
            CHECK(pc_dispatch_next_response(19U, &response) == PC_DISPATCH_RESPONSE_READY);
            body = decode(&response, PC_COMMAND_TEST_RESULT_REPORT, 19U);
            string_is(body, "algorithmName", algorithm->name);
            string_is(body, "siteId", "SITE-7");
            number_is(body, "algorithmIndex", 37U);
            number_is(body, "testTimeMs", 42000U);
            const cJSON *results = cJSON_GetObjectItemCaseSensitive(body, "testResults");
            CHECK(cJSON_GetArraySize(results) == 2);
            for (unsigned die = 0U; die < 2U; ++die) {
                const cJSON *chip = cJSON_GetArrayItem(results, die);
                uint8_t injected = id == 17U && die == 0U;
                uint8_t failed = (sample & (1U << die)) != 0U;
                number_is(chip, "testStatus", injected || failed ? 0U : 1U);
                number_is(chip, "mirrorIndex", die);
                CHECK(!cJSON_GetObjectItemCaseSensitive(chip, "siteId"));
                CHECK(!cJSON_GetObjectItemCaseSensitive(chip, "measurementData"));
                string_is(chip, "physicalAddr", "");
                const cJSON *desc = cJSON_GetObjectItemCaseSensitive(chip, "resultDesc");
                if (failed) string_is(desc, "firstErrorAddress", die ? "0xFFFFFFFFFFFFFFFF" : "0x0000000000000001");
                else if (injected) string_is(desc, "firstErrorAddress", "0x0000000012345678");
                else CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(desc, "firstErrorAddress")));
                string_is(desc, "errorType", injected ? "SAF" : "");
            }
            CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(body, "measurementData")) == 18);
            CHECK(memcmp(&queued_response.result, &original, sizeof(original)) == 0);
            cJSON_Delete(body);
            CHECK(!pc_test_service_busy() && pc_dispatch_next_response(20U, &response) == 0);
        }
        cJSON_free(json);
        cJSON_Delete(request);
    }
    /* 未收到完整结果、超时、串口发送失败或失去原连接，均不能伪造0105。 */
    start_expect(START_JSON, NULL);
    inject(RK_RESPONSE_TIMEOUT, 0U);
    CHECK(pc_dispatch_next_response(19U, &response) == PC_DISPATCH_RESPONSE_READY);
    status_is(&response, "timeout", 19U);
    CHECK(pc_test_service_busy());
    start_expect(START_JSON, "busy");
    inject(RK_RESPONSE_RECOVERED, 0U);
    CHECK(pc_dispatch_next_response(20U, &response) == 0 && !pc_test_service_busy());
    start_expect(START_JSON, NULL);
    inject(RK_RESPONSE_TX_FAILED, 0U);
    CHECK(pc_dispatch_next_response(21U, &response) == PC_DISPATCH_RESPONSE_READY);
    status_is(&response, "txFailed", 21U);
    start_expect(START_JSON, NULL);
    pc_dispatch_disconnected();
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(22U, &response) == 0 && !pc_test_service_busy());
    message("PASS: PC RK service MATS FAIL only; 80 results, preserved raw data, real completion and lifecycle guards\n");
}
#endif

#if defined(PC_POWER_SEQUENCE_TEST)
/* 下面的短等待只为故障注入，绝不是B1获批的硬件时序。 */
static const pc_power_config_t sequence_config = {
    .transition_timeout_ms = 100U, .ready_timeout_ms = 100U,
    .off_hold_ms = 10U, .boot_settle_ms = 5U,
};
#define COLD_START "{\"algorithmType\":\"functional\",\"algorithmName\":\"mats\",\"algorithmIndex\":37,\"power\":{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1800\",\"vdd2\":\"1100\",\"vddq\":\"600\"},\"reboot\":false,\"bootMode\":\"emmc\",\"TimingParameters\":{}}"
#define COLD_CHANGED "{\"algorithmType\":\"functional\",\"algorithmName\":\"mats\",\"algorithmIndex\":37,\"power\":{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1810\",\"vdd2\":\"1100\",\"vddq\":\"600\"},\"reboot\":false,\"bootMode\":\"emmc\",\"TimingParameters\":{}}"
static uint32_t armed_token;
static uint64_t heartbeat_seq = 100U;
static int arm_error;
static unsigned rk_arms;

static int power_rk_begin(uint32_t token)
{
    CHECK(token != 0U && token != armed_token);
    armed_token = token; ++rk_arms;
    return arm_error ? arm_error : rk_link_ready_begin(token);
}
static const pc_power_rk_adapter_t sequence_rk = {power_rk_begin, rk_link_ready_poll};
static void confirm_power(int answer)
{
    CHECK(answer == 1);
    tlv_frame_t heartbeat = {0};
    heartbeat.sequence = ++heartbeat_seq;
    rk_link_ready_note_tx(heartbeat.sequence);
    rk_link_ready_note_rx(&heartbeat);
}
static void step_power(uint32_t ms) { mock_tick += ms; pc_dispatch_poll(); }
static void configure_power(void)
{
    CHECK(!pc_test_service_busy() && !pc_power_service_busy());
    mock_power_state = PWR_CTRL_STATE_OFF;
    power_failure = PWR_CTRL_OK; boot_failure = arm_error = 0; boot_io_ms = 0U;
    CHECK(pc_power_service_configure(&sequence_config, &sequence_rk) == 0);
}
static void power_command(uint16_t command, const char *status, const char *error)
{
    pc_dispatch_response_t response;
    CHECK(dispatch(command, "{}", 101U, &response) == 1);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 101U);
    number_is(body, "requestCommandId", command);
    string_is(body, "status", status);
    if (error) string_is(body, "errorCode", error);
    cJSON_Delete(body);
}
static void power_completion(uint16_t command, const char *error)
{
    pc_dispatch_response_t response;
    CHECK(pc_dispatch_next_response(102U, &response) == 1);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 102U);
    number_is(body, "requestCommandId", command);
    string_is(body, "status", error ? "failed" : "completed");
    if (error) string_is(body, "errorCode", error);
    cJSON_Delete(body);
    CHECK(pc_dispatch_next_response(103U, &response) == 0);
}
static void preparation_failed(const char *error)
{
    pc_dispatch_response_t response;
    CHECK(pc_dispatch_next_response(104U, &response) == 1);
    status_is(&response, error, 104U);
    CHECK(!pc_test_service_busy() && !pc_power_service_busy());
}
static void cold_to_ready(void)
{
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_OFF_HOLD);
    unsigned before = power_on_calls;
    step_power(9U); CHECK(mock_power_state == PWR_CTRL_STATE_OFF);
    step_power(1U); CHECK(pc_power_service_diagnostic()->phase == PC_POWER_BOOT_SETTLE);
    step_power(4U); CHECK(power_on_calls == before);
    step_power(1U); CHECK(power_on_calls == before + 1U);
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_WAIT_ON);
    mock_power_state = PWR_CTRL_STATE_ON; step_power(1U);
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_WAIT_RK_READY);
}
static void ready_starts_algorithm(void)
{
    pc_dispatch_response_t response;
    unsigned before = queue_calls;
    confirm_power(1); step_power(0U);
    CHECK(queue_calls == before); /* 无响应缓冲的后台poll不能吞掉受理连接的状态。 */
    CHECK(pc_dispatch_next_response(105U, &response) == 1);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 105U);
    string_is(body, "status", "completed"); number_is(body, "algorithmIndex", 37U);
    cJSON_Delete(body);
    CHECK(queue_calls == before + 1U && queued_request.command_id == 17U);
}
static void direct_off_to_hold(void)
{
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_REQUEST_OFF);
    unsigned before = power_off_calls;
    step_power(0U); /* 不注入任何RK关机应答，也必须调用真实下电入口。 */
    CHECK(power_off_calls == before + 1U && pc_power_service_diagnostic()->phase == PC_POWER_WAIT_OFF);
    mock_power_state = PWR_CTRL_STATE_OFF; step_power(1U);
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_OFF_HOLD);
}

static void config_cache_check(void)
{
    static const struct {
        uint16_t command;
        int driver_result;
        const char *error;
        uint8_t preserves_ready;
    } cases[] = {
        {PC_COMMAND_SET_BOOT_MODE, MONITOR_BOOT_ERR_STATE, "invalidState", 1U},
        {PC_COMMAND_SET_BOOT_MODE, MONITOR_BOOT_ERR_BUSY, "busy", 1U},
        {PC_COMMAND_SET_BOOT_MODE, MONITOR_BOOT_ERR_PARAM, "invalidArgument", 1U},
        {PC_COMMAND_SET_BOOT_MODE, MONITOR_BOOT_OK, NULL, 0U},
        {PC_COMMAND_SET_BOOT_MODE, MONITOR_BOOT_ERR_IO, "executionFailed", 0U},
        {PC_COMMAND_SET_BOOT_MODE, MONITOR_BOOT_ERR_NOT_SET, "executionFailed", 0U},
        {PC_COMMAND_SET_DDR_CONFIG, PWR_CTRL_ERR_PARAM, "invalidArgument", 1U},
        {PC_COMMAND_SET_DDR_CONFIG, PWR_CTRL_ERR_NOT_CONFIGURED, "executionFailed", 1U},
        {PC_COMMAND_SET_DDR_CONFIG, PWR_CTRL_ERR_BUSY, "invalidState", 1U},
        {PC_COMMAND_SET_DDR_CONFIG, PWR_CTRL_ERR_RESOURCE_BUSY, "busy", 1U},
        {PC_COMMAND_SET_DDR_CONFIG, PWR_CTRL_OK, NULL, 0U},
        {PC_COMMAND_SET_DDR_CONFIG, PWR_CTRL_ERR_FAULT, "executionFailed", 0U},
    };
    const char *boot_json = "{\"bootMode\":\"emmc\"}";
    const char *ddr_json = "{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1800\",\"vdd2\":\"1100\",\"vddq\":\"600\"}";
    pc_dispatch_response_t response;
    configure_power();
    start_expect(COLD_START, NULL); cold_to_ready(); ready_starts_algorithm();
    result_is(0U, NULL, "SITE-7");

    for (unsigned i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        unsigned on = power_on_calls, off = power_off_calls, boots = boot_calls;
        unsigned arms = rk_arms, queued = queue_calls, calls = config_set_calls;
        config_result_override = 1;
        config_boot_result = (monitor_boot_result_t)cases[i].driver_result;
        config_ddr_result = (pwr_ctrl_result_t)cases[i].driver_result;
        CHECK(dispatch(cases[i].command,
            cases[i].command == PC_COMMAND_SET_BOOT_MODE ? boot_json : ddr_json,
            106U, &response) == PC_DISPATCH_RESPONSE_READY);
        config_result_override = 0;
        CHECK(config_set_calls == calls + 1U);
        cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 106U);
        number_is(body, "requestCommandId", cases[i].command);
        string_is(body, "status", cases[i].error ? "failed" : "completed");
        if (cases[i].error) string_is(body, "errorCode", cases[i].error);
        else CHECK(!cJSON_GetObjectItemCaseSensitive(body, "errorCode"));
        cJSON_Delete(body);

        /* 拒绝或写入结果不自动重启；下一次0103才决定复用还是重新准备。 */
        CHECK(power_on_calls == on && power_off_calls == off && boot_calls == boots);
        CHECK(rk_arms == arms && queue_calls == queued);
        start_expect(COLD_START, NULL);
        if (cases[i].preserves_ready) {
            CHECK(pc_power_service_diagnostic()->phase == PC_POWER_DONE);
            CHECK(pc_dispatch_next_response(107U, &response) == PC_DISPATCH_RESPONSE_READY);
            body = decode(&response, PC_COMMAND_COMMAND_RESP, 107U);
            string_is(body, "status", "completed"); cJSON_Delete(body);
            CHECK(power_on_calls == on && power_off_calls == off && boot_calls == boots);
            CHECK(rk_arms == arms && queue_calls == queued + 1U);
        } else {
            /* 成功写入、IO故障或未约定错误均不能继续信任旧成功标记。 */
            CHECK(pc_power_service_diagnostic()->phase == PC_POWER_REQUEST_OFF);
            direct_off_to_hold(); cold_to_ready(); ready_starts_algorithm();
            CHECK(power_on_calls == on + 1U && power_off_calls == off + 1U);
            CHECK(boot_calls == boots + 1U && rk_arms == arms + 1U);
        }
        result_is(0U, NULL, "SITE-7");
    }
    message("PASS: PC RK service 0506/0509 reject-preserves-ready; success/uncertain-write invalidates; same-target 0103 reuse\n");
}

static void power_sequence_check(void)
{
    pc_dispatch_response_t response;
    power_fixture = 1;
    start_expect(COLD_START, "notConfigured");
    CHECK(!power_on_calls && !power_off_calls && !boot_calls && !queue_calls);
    pc_power_config_t invalid = sequence_config;
    invalid.transition_timeout_ms = 0U;
    CHECK(pc_power_service_configure(&invalid, &sequence_rk) == -1);
    invalid = sequence_config; invalid.ready_timeout_ms = UINT32_MAX;
    CHECK(pc_power_service_configure(&invalid, &sequence_rk) == -1);
    CHECK(pc_power_service_configure(&sequence_config, NULL) == 0);
    start_expect(COLD_START, "notReady");
    configure_power();
    CHECK(!power_on_calls && !power_off_calls && !boot_calls && !queue_calls);
    boot_io_ms = 400U; /* 模拟I2C耗时，后续5ms稳定时间必须从写完开始算。 */
    start_expect(COLD_START, NULL);
    CHECK(queue_calls == 0U && pc_power_service_busy());
    CHECK(pc_power_service_configure(&sequence_config, &sequence_rk) == -1);
    start_expect(START_JSON, "busy");
    power_command(PC_COMMAND_POWER_OFF, "failed", "busy");
    CHECK(dispatch(PC_COMMAND_CLIENT_HEARTBEAT, "{}", 1U, &response) == 1);
    CHECK(dispatch(PC_COMMAND_QUERY_POWER_STATUS, "{}", 2U, &response) == 1);
    power_command(PC_COMMAND_FIRMWARE_UPGRADE_REQUEST, "failed", "notSupported");
    cold_to_ready();
    /* 未发出本轮MCU心跳前，旧心跳不能放行；旧内部令牌同样失效。 */
    tlv_frame_t stale = {0};
    stale.sequence = heartbeat_seq;
    rk_link_ready_note_rx(&stale);
    CHECK(rk_link_ready_poll(armed_token - 1U) == -1);
    step_power(1U); CHECK(queue_calls == 0U);
    ready_starts_algorithm(); result_is(0U, NULL, "SITE-7");

    /* 同配置且reboot=false复用已确认启动，不再次操作DAC/电源。 */
    unsigned on = power_on_calls, off = power_off_calls, boots = boot_calls, arms = rk_arms;
    start_expect(COLD_START, NULL);
    CHECK(pc_dispatch_next_response(3U, &response) == 1);
    cJSON *done = decode(&response, PC_COMMAND_COMMAND_RESP, 3U);
    string_is(done, "status", "completed"); cJSON_Delete(done);
    CHECK(power_on_calls == on && power_off_calls == off && boot_calls == boots && rk_arms == arms);
    result_is(0U, NULL, "SITE-7");
    /* 改电压，即使PC没要求reboot，MCU也必须直接关电后冷启动。 */
    start_expect(COLD_CHANGED, NULL);
    direct_off_to_hold(); cold_to_ready(); ready_starts_algorithm();
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDD1) == 1810U);
    result_is(0U, NULL, "SITE-7");
    /* 0505保留训练目标而非运行期缓存，并等新的RK READY后回completed。 */
    power_command(PC_COMMAND_RESET_RK, "accepted", NULL);
    direct_off_to_hold(); cold_to_ready(); confirm_power(1); step_power(0U);
    power_completion(PC_COMMAND_RESET_RK, NULL);
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDD1) == 1810U);

    /* 下电不依赖RK在线，驱动失败只报一次；低层OFF等待覆盖tick回绕。 */
    off = power_off_calls; mock_tick = UINT32_MAX - 50U;
    power_failure = PWR_CTRL_ERR_RESOURCE_BUSY;
    power_command(PC_COMMAND_POWER_OFF, "accepted", NULL);
    step_power(0U); power_completion(PC_COMMAND_POWER_OFF, "busy");
    CHECK(mock_power_state == PWR_CTRL_STATE_ON && power_off_calls == off + 1U);
    power_failure = PWR_CTRL_OK;
    power_command(PC_COMMAND_POWER_OFF, "accepted", NULL);
    step_power(0U); step_power(99U);
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_WAIT_OFF);
    step_power(1U); power_completion(PC_COMMAND_POWER_OFF, "timeout");
    CHECK(pc_power_service_diagnostic()->failed_phase == PC_POWER_WAIT_OFF);
    mock_power_state = PWR_CTRL_STATE_OFF;
    power_command(PC_COMMAND_POWER_OFF, "completed", NULL);
    configure_power(); CHECK(pc_power_service_configure(&sequence_config, NULL) == 0);
    mock_power_state = PWR_CTRL_STATE_ON;
    power_command(PC_COMMAND_POWER_OFF, "accepted", NULL);
    step_power(0U); mock_power_state = PWR_CTRL_STATE_OFF; step_power(1U);
    power_completion(PC_COMMAND_POWER_OFF, NULL);

    /* PC负责DDR范围，1820mV不再因额外白名单而拒绝；TPS边界/步进仍拒绝。 */
    cJSON *request = cJSON_Parse(COLD_START);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(request, "power"),
        "vdd1", cJSON_CreateString("1820")));
    char *json = cJSON_PrintUnformatted(request); CHECK(json);
    configure_power(); start_expect(json, NULL);
    cold_to_ready(); ready_starts_algorithm();
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDD1) == 1820U);
    result_is(0U, NULL, "SITE-7");
    cJSON_free(json);
    const char *bad_mv[] = {"799", "3351", "1801"};
    for (unsigned i = 0U; i < sizeof(bad_mv) / sizeof(bad_mv[0]); ++i) {
        CHECK(cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(request, "power"),
            "vdd1", cJSON_CreateString(bad_mv[i])));
        json = cJSON_PrintUnformatted(request); CHECK(json);
        on = power_on_calls; off = power_off_calls;
        start_expect(json, "invalidArgument");
        CHECK(on == power_on_calls && off == power_off_calls);
        cJSON_free(json);
    }
    cJSON_Delete(request);

    /* 真实DAC失败留OFF；TPS故障不进入算法。 */
    configure_power(); on = power_on_calls; boot_failure = 1;
    start_expect(COLD_START, NULL); step_power(10U); preparation_failed("executionFailed");
    CHECK(power_on_calls == on && mock_power_state == PWR_CTRL_STATE_OFF);
    configure_power(); start_expect(COLD_START, NULL); step_power(10U); step_power(5U);
    mock_power_state = PWR_CTRL_STATE_FAULT; step_power(1U); preparation_failed("executionFailed");
    /* 锁忙/上电拒绝只报一次，不自动重试驱动或算法。 */
    configure_power(); power_failure = PWR_CTRL_ERR_RESOURCE_BUSY;
    start_expect(COLD_START, NULL); step_power(10U); step_power(5U); preparation_failed("busy");
    on = power_on_calls; step_power(1000U); CHECK(power_on_calls == on);

    /* READY超时只结束业务等待，不擅自强制切断可能仍在启动的RK。 */
    configure_power(); start_expect(COLD_START, NULL); cold_to_ready();
    unsigned queued = queue_calls;
    confirm_power(1); step_power(100U); preparation_failed("timeout");
    CHECK(queue_calls == queued && mock_power_state == PWR_CTRL_STATE_ON);
    CHECK(pc_power_service_diagnostic()->failed_phase == PC_POWER_WAIT_RK_READY);
    configure_power(); start_expect(COLD_START, NULL); step_power(10U); step_power(5U);
    step_power(100U); preparation_failed("timeout");
    CHECK(queue_calls == queued); /* 低层仍过渡时，不能自动重发或假报算法FAIL。 */

    /* 同配置reboot=true必须冷重启；准备期间断线只收尾、不再开始算法。 */
    configure_power(); start_expect(COLD_START, NULL); cold_to_ready(); ready_starts_algorithm();
    result_is(0U, NULL, "SITE-7");
    request = cJSON_Parse(COLD_START);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(request, "reboot", cJSON_CreateTrue()));
    json = cJSON_PrintUnformatted(request); CHECK(json); start_expect(json, NULL);
    cJSON_free(json); cJSON_Delete(request);
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_REQUEST_OFF);
    pc_dispatch_disconnected(); queued = queue_calls;
    direct_off_to_hold(); cold_to_ready(); confirm_power(1); step_power(0U);
    CHECK(!pc_test_service_busy() && !pc_power_service_busy() && queue_calls == queued);
    CHECK(pc_dispatch_next_response(1U, &response) == 0);
    /* 准备成功但RK队列拥堵：终止统计、报告busy，不重新开始电源循环。 */
    start_expect(COLD_START, NULL); queue_full = 1;
    preparation_failed("busy"); queue_full = 0;
    CHECK(queue_calls == queued + 1U);

    /* 启动准备已完成但应答内存不足：不把算法偷偷投递给RK。 */
    configure_power(); start_expect(COLD_START, NULL); cold_to_ready();
    confirm_power(1); step_power(0U); queued = queue_calls;
    pc_json_configure_allocator(no_memory, free);
    CHECK(pc_dispatch_next_response(1U, &response) == PC_DISPATCH_ERROR);
    pc_json_configure_allocator(NULL, NULL);
    CHECK(queue_calls == queued && !pc_test_service_busy() && !pc_power_service_busy());

    /* 0501复用真实训练上电驱动，不要求伪造READY；断线后的终态不交给新连接。 */
    configure_power(); CHECK(pc_power_service_configure(&sequence_config, NULL) == 0);
#if defined(B1_RK_UART)
    on = power_on_calls;
    config_result_override = 1; config_ddr_result = PWR_CTRL_OK;
    CHECK(dispatch(PC_COMMAND_SET_DDR_CONFIG,
        "{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1700\",\"vdd2\":\"1060\",\"vddq\":\"570\"}",
        101U, &response) == PC_DISPATCH_RESPONSE_READY);
    cJSON *preset = decode(&response, PC_COMMAND_COMMAND_RESP, 101U);
    string_is(preset, "status", "completed"); cJSON_Delete(preset);
    config_result_override = 0;
    CHECK(power_on_calls == on && mock_power_state == PWR_CTRL_STATE_OFF);
#endif
    power_command(PC_COMMAND_POWER_ON, "accepted", NULL);
    step_power(10U); step_power(5U); mock_power_state = PWR_CTRL_STATE_ON; step_power(1U);
    power_completion(PC_COMMAND_POWER_ON, NULL);
#if defined(B1_RK_UART)
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDD1) == 1700U);
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDD2) == 1060U);
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDDQ) == 570U);
#endif
    configure_power(); power_command(PC_COMMAND_POWER_ON, "accepted", NULL);
    pc_dispatch_disconnected(); step_power(10U); step_power(5U);
    mock_power_state = PWR_CTRL_STATE_ON; step_power(1U);
    CHECK(!pc_power_service_busy() && pc_dispatch_next_response(1U, &response) == 0);
    config_cache_check();
    message("PASS: PC RK service real power sequence: cold start/reuse/change/reboot, direct off, heartbeat ready, PC voltage range, guards, failures, deadlines, disconnects\n");
}

#if defined(TEST_B1_CONSOLE_POWER)
static void console_command(log_console_power_request_t request)
{
    CHECK(console_request == LOG_CONSOLE_POWER_NONE);
    console_request = request;
    pc_dispatch_poll();
    CHECK(console_request == LOG_CONSOLE_POWER_NONE);
}
static void console_cold_finish(void)
{
    unsigned completed = console_completions, on = power_on_calls, arms = rk_arms, queued = queue_calls;
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_OFF_HOLD);
    step_power(sequence_config.off_hold_ms);
    CHECK(last_boot_mode == BOOT_MODE_FSPI_M1 && power_on_calls == on);
    step_power(sequence_config.boot_settle_ms);
    CHECK(power_on_calls == on + 1U && mock_power_state == PWR_CTRL_STATE_WAIT_FIRST_RAIL);
    CHECK(ddr_power_get_memory_type() == DDR_MEMORY_LPDDR4X);
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDD1) == 1800U);
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDD2) == 1100U);
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDDQ) == 600U);
    CHECK(console_completions == completed);
    mock_power_state = PWR_CTRL_STATE_ON; step_power(1U);
    CHECK(console_completions == completed + 1U && console_error == NULL);
    CHECK(!pc_power_service_busy() && !pc_test_service_busy());
    CHECK(rk_arms == arms && queue_calls == queued); /* 不等待SPL心跳、不发算法或建SLT占用。 */
}
static void console_power_check(void)
{
    pc_dispatch_response_t response;
    configure_power();
    CHECK(pc_power_service_configure(NULL, NULL) == 0);
    console_command(LOG_CONSOLE_POWER_START_RK);
    CHECK(strcmp(console_error, "notConfigured") == 0 && !pc_power_service_busy());

    configure_power();
    const uint16_t biased[] = {1700U, 1060U, 570U};
    CHECK(ddr_power_configure_training(DDR_MEMORY_LPDDR4X, biased) == IF_OK);
    CHECK(pc_power_service_configure(&sequence_config, NULL) == 0);
    console_command(LOG_CONSOLE_POWER_START_RK);
    power_command(PC_COMMAND_POWER_OFF, "failed", "busy");
    start_expect(START_JSON, "busy");
    pc_dispatch_disconnected(); /* 无PC/断线均不取消已经受理的本地动作。 */
    console_cold_finish();
    CHECK(pc_dispatch_next_response(500U, &response) == 0);

    /* 已经ON也重新下电，确保Boot和训练目标真正重新进入启动流程。 */
    console_command(LOG_CONSOLE_POWER_START_RK);
    direct_off_to_hold(); console_cold_finish();
    console_command(LOG_CONSOLE_POWER_END_RK);
    step_power(0U);
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_WAIT_OFF);
    unsigned completed = console_completions, off = power_off_calls;
    mock_power_state = PWR_CTRL_STATE_OFF; step_power(1U);
    CHECK(console_completions == completed + 1U && console_error == NULL && !pc_power_service_busy());
    console_command(LOG_CONSOLE_POWER_END_RK); step_power(0U);
    CHECK(power_off_calls == off && console_error == NULL && !pc_power_service_busy());

    /* PC电源结果仍由原连接消费，本地请求不能抢走其完成事件。 */
    configure_power(); power_command(PC_COMMAND_POWER_ON, "accepted", NULL);
    console_command(LOG_CONSOLE_POWER_END_RK);
    CHECK(strcmp(console_error, "busy") == 0);
    step_power(10U); step_power(5U); mock_power_state = PWR_CTRL_STATE_ON; step_power(1U);
    power_completion(PC_COMMAND_POWER_ON, NULL);
    start_expect(START_JSON, NULL);
    off = power_off_calls;
    console_command(LOG_CONSOLE_POWER_END_RK);
    CHECK(strcmp(console_error, "busy") == 0 && pc_test_service_busy() && power_off_calls == off);
    result_is(0U, NULL, "SITE-7");

    /* DAC失败、供电失败和等待超时均报告一次，不补发或假报ON。 */
    configure_power(); boot_failure = 1;
    unsigned on = power_on_calls;
    console_command(LOG_CONSOLE_POWER_START_RK); step_power(10U);
    CHECK(strcmp(console_error, "executionFailed") == 0 && !pc_power_service_busy() && power_on_calls == on);
    configure_power(); power_failure = PWR_CTRL_ERR_FAULT;
    console_command(LOG_CONSOLE_POWER_START_RK); step_power(10U); step_power(5U);
    CHECK(strcmp(console_error, "executionFailed") == 0 && !pc_power_service_busy());
    configure_power(); console_command(LOG_CONSOLE_POWER_START_RK); step_power(10U); step_power(5U);
    step_power(sequence_config.transition_timeout_ms);
    CHECK(strcmp(console_error, "timeout") == 0 && !pc_power_service_busy());
    completed = console_completions; step_power(1000U);
    CHECK(console_completions == completed);
    configure_power();
    message("PASS: console start/end RK: nominal training, fspiM1, cold boot, no heartbeat/algorithm, PC mutual exclusion, disconnect and failures\n");
}
#endif
#endif

#if defined(PC_TEST_ROUND_TEST)
#define SLT_START "{\"algorithmType\":\"slt\",\"algorithmName\":\"memtest86\",\"algorithmIndex\":null,\"power\":{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1800\",\"vdd2\":\"1100\",\"vddq\":\"600\"},\"reboot\":false,\"bootMode\":\"emmc\",\"TimingParameters\":{}}"
#define SLT_RESULT "{\"siteId\":\"SITE-7\",\"algorithmType\":\"slt\",\"algorithmName\":\"memtest86\",\"algorithmIndex\":null,\"testTimeMs\":null,\"testResults\":[{\"mirrorIndex\":0,\"testStatus\":0,\"resultDesc\":null,\"physicalAddr\":\"\"},{\"mirrorIndex\":1,\"testStatus\":1,\"resultDesc\":null,\"physicalAddr\":\"\"}],\"measurementData\":null}"

static void round_command(uint16_t command, const char *json, const char *status, const char *error)
{
    pc_dispatch_response_t response;
    CHECK(dispatch(command, json, 77U, &response) == PC_DISPATCH_RESPONSE_READY);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 77U);
    number_is(body, "requestCommandId", command);
    string_is(body, "status", status);
    if (error) string_is(body, "errorCode", error);
    else CHECK(!cJSON_GetObjectItemCaseSensitive(body, "errorCode"));
    if (command == PC_COMMAND_SLT_START || command == PC_COMMAND_SLT_RESULT)
        CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(body, "algorithmIndex")));
    cJSON_Delete(body);
}

static void round_begin(void) { round_command(PC_COMMAND_ROUND_BEGIN, "{}", "completed", NULL); }
static void round_end(void)
{
    round_command(PC_COMMAND_ROUND_END, "{\"errorchip\":[]}", "completed", NULL);
}

/* 构造PC实际请求文本；返回后cJSON源树已销毁，业务不能借用临时字段。 */
static char *para_json(const char *type, const char *name, uint32_t index)
{
    cJSON *root = cJSON_Parse(START_JSON); CHECK(root);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(root, "algorithmType", cJSON_CreateString(type)));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(root, "algorithmName", cJSON_CreateString(name)));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(root, "algorithmIndex", cJSON_CreateNumber(index)));
    cJSON *timing = cJSON_GetObjectItemCaseSensitive(root, "TimingParameters"); CHECK(cJSON_IsObject(timing));
    CHECK(cJSON_AddStringToObject(timing, "Freq", ""));
    CHECK(cJSON_AddStringToObject(timing, "tRCD", ""));
    CHECK(cJSON_AddStringToObject(timing, "CASReadLatency", ""));
    CHECK(cJSON_AddStringToObject(timing, "CASWriteLatency", ""));
    char *json = cJSON_PrintUnformatted(root); CHECK(json);
    cJSON_Delete(root);
    return json;
}

static void para_command(uint16_t command, const char *json, uint32_t index, const char *error)
{
    pc_dispatch_response_t response;
    unsigned calls = queue_calls;
    CHECK(dispatch(command, json, 61U, &response) == PC_DISPATCH_RESPONSE_READY);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 61U);
    number_is(body, "requestCommandId", command);
    number_is(body, "algorithmIndex", index);
    string_is(body, "status", error ? "failed" : "accepted");
    if (error) {
        string_is(body, "errorCode", error);
        CHECK(queue_calls == calls);
    } else CHECK(!cJSON_GetObjectItemCaseSensitive(body, "errorCode"));
    cJSON_Delete(body);
}

/* 只验证编号下发与最终帧转发；这里没有伪造RK进入确认或JEDEC有效测量窗口。 */
static void para_flow_check(void)
{
    static const char *const names[] = {
        "idd_0", "idd_2P", "idd_2PS", "idd_2N", "idd_2NS", "idd_3P", "idd_3PS", "idd_3N",
        "idd_3NS", "idd_4R", "idd_4W", "idd_5", "idd_5AB", "idd_5PB", "idd_6", "idd_6ET"
    };
    pc_dispatch_response_t response;
    char *json = para_json("parametric", "idd_0", 91U);
    CHECK(!pc_test_service_busy());
    para_command(PC_COMMAND_PARA_START, json, 91U, "invalidState");
    round_begin();
    para_command(PC_COMMAND_START_TEST, json, 91U, "notSupported");
    cJSON_free(json);
    const struct { uint16_t command; const char *type; const char *name; } rejected[] = {
        {PC_COMMAND_START_TEST, "functional", "idd_0"},
        {PC_COMMAND_PARA_START, "functional", "idd_0"},
        {PC_COMMAND_PARA_START, "functional", "mats"},
        {PC_COMMAND_PARA_START, "parametric", "mats"},
        {PC_COMMAND_PARA_START, "parametric", "idd_2p"},
        {PC_COMMAND_PARA_START, "parametric", "IDD_0"}
    };
    for (size_t i = 0U; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        json = para_json(rejected[i].type, rejected[i].name, 91U);
        para_command(rejected[i].command, json, 91U, "notSupported");
        cJSON_free(json);
        CHECK(!pc_test_service_busy());
    }
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        json = para_json("parametric", names[i], 100U + i);
        unsigned calls = queue_calls;
        para_command(PC_COMMAND_PARA_START, json, 100U + i, NULL);
        CHECK(queue_calls == calls + 1U && queued_request.command_id == 21U + i);
        CHECK(queued_request.timeout_ms == 0U && pc_test_service_busy());
        para_command(PC_COMMAND_PARA_START, json, 100U + i, "busy");
        round_command(PC_COMMAND_START_TEST, START_JSON, "failed", "busy");
        cJSON_free(json);
        string_is(pc_test_service_timing_parameters(), "Freq", "");
        string_is(pc_test_service_timing_parameters(), "tRCD", "");
        uint8_t mask = i % 4U;
        inject_mask(RK_RESPONSE_COMPLETE, mask, UINT64_C(0x1234), UINT64_C(0x9876));
        CHECK(pc_dispatch_next_response(62U, &response) == 1 && !pc_test_service_busy());
        cJSON *body = decode(&response, PC_COMMAND_PARA_RESULT, 62U);
        string_is(body, "algorithmType", "parametric");
        string_is(body, "algorithmName", names[i]);
        string_is(body, "siteId", site_label);
        number_is(body, "algorithmIndex", 100U + i);
        number_is(body, "testTimeMs", 42000U);
        CHECK(cJSON_GetObjectItemCaseSensitive(body, "measurementData"));
        const cJSON *chips = cJSON_GetObjectItemCaseSensitive(body, "testResults");
        CHECK(cJSON_GetArraySize(chips) == 2);
        for (unsigned die = 0U; die < 2U; ++die) {
            const cJSON *chip = cJSON_GetArrayItem(chips, die);
            number_is(chip, "mirrorIndex", die);
            number_is(chip, "testStatus", mask & (1U << die) ? 0U : 1U);
            const cJSON *desc = cJSON_GetObjectItemCaseSensitive(chip, "resultDesc");
            string_is(desc, "errorType", "");
            if (mask & (1U << die))
                string_is(desc, "firstErrorAddress", die ? "0x0000000000009876" : "0x0000000000001234");
            else CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(desc, "firstErrorAddress")));
        }
        cJSON_Delete(body);
    }

#if defined(PC_POWER_SEQUENCE_TEST)
    /* 完整PARA启动同样等待本轮RK心跳，异步completed仍须关联0108，不能串到0103。 */
    power_fixture = 1;
    configure_power();
    cJSON *cold = cJSON_Parse(COLD_START); CHECK(cold);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(cold, "algorithmType", cJSON_CreateString("parametric")));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(cold, "algorithmName", cJSON_CreateString("idd_0")));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(cold, "algorithmIndex", cJSON_CreateNumber(91)));
    json = cJSON_PrintUnformatted(cold); CHECK(json); cJSON_Delete(cold);
    unsigned queued = queue_calls;
    para_command(PC_COMMAND_PARA_START, json, 91U, NULL);
    cJSON_free(json);
    CHECK(queue_calls == queued);
    cold_to_ready(); confirm_power(1); step_power(0U);
    CHECK(pc_dispatch_next_response(62U, &response) == 1);
    cJSON *completed = decode(&response, PC_COMMAND_COMMAND_RESP, 62U);
    number_is(completed, "requestCommandId", PC_COMMAND_PARA_START);
    number_is(completed, "algorithmIndex", 91U);
    string_is(completed, "status", "completed"); cJSON_Delete(completed);
    CHECK(queue_calls == queued + 1U && queued_request.command_id == 21U);
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(62U, &response) == 1 && !pc_test_service_busy());
    completed = decode(&response, PC_COMMAND_PARA_RESULT, 62U); cJSON_Delete(completed);
#endif

    /* 传输异常不能被写成IDD芯片FAIL；超时未知状态仍等待真实恢复事件。 */
    json = para_json("parametric", "idd_0", 91U);
    const rk_response_status_t errors[] = {RK_RESPONSE_TX_FAILED, RK_RESPONSE_TIMEOUT};
    const char *const error_names[] = {"txFailed", "timeout"};
    for (size_t i = 0U; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        para_command(PC_COMMAND_PARA_START, json, 91U, NULL);
        inject(errors[i], 0U);
        CHECK(pc_dispatch_next_response(63U, &response) == 1);
        cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 63U);
        number_is(body, "requestCommandId", PC_COMMAND_PARA_START);
        number_is(body, "algorithmIndex", 91U);
        string_is(body, "status", "failed"); string_is(body, "errorCode", error_names[i]);
        CHECK(!cJSON_GetObjectItemCaseSensitive(body, "testResults"));
        cJSON_Delete(body);
        if (errors[i] == RK_RESPONSE_TIMEOUT) {
            CHECK(pc_test_service_busy());
            inject(RK_RESPONSE_RECOVERED, 0U);
            CHECK(pc_dispatch_next_response(64U, &response) == 0);
        }
        CHECK(!pc_test_service_busy());
    }
    /* 原连接离开或整轮结束，只废弃结果，不能提前解除RK占用。 */
    para_command(PC_COMMAND_PARA_START, json, 91U, NULL);
    pc_dispatch_disconnected(); CHECK(pc_test_service_busy());
    para_command(PC_COMMAND_PARA_START, json, 91U, "busy");
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(65U, &response) == 0 && !pc_test_service_busy());
    round_begin();
    para_command(PC_COMMAND_PARA_START, json, 91U, NULL);
    round_end(); CHECK(pc_test_service_busy());
    unsigned writes = led_writes;
    inject(RK_RESPONSE_COMPLETE, UINT64_C(0x1234));
    CHECK(pc_dispatch_next_response(66U, &response) == 0 && !pc_test_service_busy());
    CHECK(led_writes == writes);
    cJSON_free(json);
    message("PASS: PC RK service PARA 16 names, category isolation, 0108/0109 routing, ownership and errors\n");
}

static void round_flow_check(void)
{
    pc_dispatch_response_t response;
#if defined(B1_RK_UART)
    mock_power_state = PWR_CTRL_STATE_ON;
#endif
    round_command(PC_COMMAND_START_TEST, START_JSON, "failed", "invalidState");
    round_command(PC_COMMAND_ROUND_END, "{\"errorchip\":[]}", "failed", "invalidState");
    round_command(PC_COMMAND_ROUND_BEGIN, "{\"heating\":false}", "failed", "invalidArgument");
    CHECK(!led_writes);
    led_failure = 1;
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "failed", "executionFailed");
    led_failure = 0;
    pc_json_configure_allocator(no_memory, free);
    CHECK(pc_test_round_begin(77U, &response) == PC_DISPATCH_ERROR);
    pc_json_configure_allocator(NULL, NULL);
    CHECK(!led_writes);
    round_begin();
    for (unsigned i = 0U; i < RESULT_LED_TARGET_COUNT; ++i) CHECK(led_states[i] == RESULT_LED_TEST_NORMAL);
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "failed", "busy");
    const char *bad_lists[] = {
        "{}", "{\"errorchip\":null}", "{\"errorchip\":{},\"leds\":{}}",
        "{\"errorchip\":[{\"physicalAddr\":\"a\",\"mirrorIndex\":0},{\"physicalAddr\":\"b\",\"mirrorIndex\":3}]}",
        "{\"errorchip\":[{\"physicalAddr\":\"\",\"mirrorIndex\":-1}]}",
        "{\"errorchip\":[{\"physicalAddr\":\"\",\"mirrorIndex\":1.5}]}",
        "{\"errorchip\":[{\"physicalAddr\":\"\",\"mirrorindex\":1}]}",
        "{\"errorchip\":[{\"physicalAddr\":3,\"mirrorIndex\":1}]}",
        "{\"errorchip\":[{\"physicalAddr\":\"\",\"mirrorIndex\":\"1\"}]}",
        "{\"errorchip\":[{\"physicalAddr\":\"\",\"mirrorIndex\":0,\"mirrorIndex\":1}]}"
    };
    unsigned writes = led_writes;
    for (unsigned i = 0U; i < sizeof(bad_lists) / sizeof(bad_lists[0]); ++i) {
        round_command(PC_COMMAND_ROUND_END, bad_lists[i], "failed", "invalidArgument");
        CHECK(led_writes == writes);
    }
    /* 旧070x不论方向都不执行；PARA开始必须携带完整字段，结果仍仅允许MCU发送。 */
    for (uint16_t command = 0x0701U; command <= 0x0703U; ++command)
        round_command(command, "{}", "failed", "notSupported");
    round_command(PC_COMMAND_PARA_START, "{}", "failed", "invalidArgument");
    round_command(PC_COMMAND_PARA_RESULT, "{}", "failed", "invalidDirection");
    round_command(PC_COMMAND_TEST_RESULT_REPORT, "{}", "failed", "invalidDirection");
    CHECK(led_writes == writes);
    pc_json_configure_allocator(no_memory, free);
    CHECK(pc_test_round_end(7U, 77U, &response) == PC_DISPATCH_ERROR);
    pc_json_configure_allocator(NULL, NULL);
    CHECK(led_writes == writes);
    led_failure = 1;
    round_command(PC_COMMAND_ROUND_END, "{\"errorchip\":[]}", "failed", "executionFailed");
    led_failure = 0;
    /* 同镜像多颗、重复芯片都只归并到一盏FAIL灯，不要求PC发送固定三灯对象。 */
    round_command(PC_COMMAND_ROUND_END,
        "{\"errorchip\":[{\"physicalAddr\":\"a\",\"mirrorIndex\":0},{\"physicalAddr\":\"b\",\"mirrorIndex\":0},{\"physicalAddr\":\"b\",\"mirrorIndex\":0},{\"physicalAddr\":\"\",\"mirrorIndex\":2}]}",
        "completed", NULL);
    CHECK(led_states[0] == RESULT_LED_FAIL && led_states[1] == RESULT_LED_PASS && led_states[2] == RESULT_LED_FAIL);
    round_command(PC_COMMAND_ROUND_END, "{\"errorchip\":[]}", "failed", "invalidState");
    round_begin();
    round_command(PC_COMMAND_START_TEST, START_JSON, "accepted", NULL);
    round_end(); writes = led_writes;
    CHECK(pc_test_service_busy());
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "failed", "busy");
    round_command(PC_COMMAND_POWER_OFF, "{}", "failed", "busy");
    round_command(PC_COMMAND_FIRMWARE_UPGRADE_REQUEST, "{\"action\":\"mcu_begin\"}", "failed", "notSupported");
    CHECK(dispatch(PC_COMMAND_CLIENT_HEARTBEAT, "{}", 77U, &response) == 1);
    inject(RK_RESPONSE_COMPLETE, UINT64_C(0x123456789ABCDEF0));
    CHECK(pc_dispatch_next_response(78U, &response) == 0 && !pc_test_service_busy());
    CHECK(led_writes == writes);
    round_command(PC_COMMAND_START_TEST, START_JSON, "failed", "invalidState");
    round_begin();
    round_command(PC_COMMAND_START_TEST, START_JSON, "accepted", NULL);
    inject(RK_RESPONSE_TIMEOUT, 0U);
    CHECK(pc_dispatch_next_response(78U, &response) == 1);
    round_end();
    CHECK(pc_test_service_busy());
    inject(RK_RESPONSE_RECOVERED, 0U);
    CHECK(pc_dispatch_next_response(79U, &response) == 0 && !pc_test_service_busy());
    round_begin(); pc_dispatch_disconnected();
    round_command(PC_COMMAND_START_TEST, START_JSON, "failed", "invalidState");
    round_begin(); round_end();
    /* B2模式独立于轮次保存：未知灭灯、局部更新，结束后不能覆盖PASS/FAIL。 */
    b2_known = 1U; b2_modes[0] = 1U;
    round_begin();
    CHECK(led_states[0] == RESULT_LED_TEST_HOT && led_states[1] == RESULT_LED_OFF &&
          led_states[2] == RESULT_LED_OFF);
    const uint8_t update[] = {0U, 1U, 0U};
    CHECK(pc_test_round_update_modes(2U, update));
    CHECK(led_states[0] == RESULT_LED_TEST_HOT && led_states[1] == RESULT_LED_TEST_HOT &&
          led_states[2] == RESULT_LED_OFF);
    writes = led_writes;
    CHECK(!pc_test_round_update_modes(8U, update) && led_writes == writes);
    led_failure = 1;
    CHECK(!pc_test_round_update_modes(4U, update) && led_writes == writes);
    led_failure = 0;
    round_end(); writes = led_writes;
    CHECK(pc_test_round_update_modes(1U, update) && led_writes == writes);
    CHECK(led_states[0] == RESULT_LED_PASS);
    b2_known = 7U; memset(b2_modes, 0, sizeof(b2_modes));
    message("PASS: PC RK service round lifecycle, atomic list validation, late result discard, busy, removed 070x\n");
    message("PASS: B2 partial thermal display, unknown OFF and final-result preservation\n");
}

static int timing_allocation_balance;
static void *timing_allocator(size_t bytes)
{
    void *memory = delivery_allocator(bytes);
    if (memory) ++timing_allocation_balance;
    return memory;
}
static void timing_free(void *memory)
{
    if (memory) --timing_allocation_balance;
    free(memory);
}

/* 缺失/null时序必须在受理之前拒绝，三类请求均不能改变已有时序、电源或RK队列。 */
static void timing_required_check(void)
{
    const uint16_t commands[] = {PC_COMMAND_START_TEST, PC_COMMAND_PARA_START, PC_COMMAND_SLT_START};
    const cJSON *saved = pc_test_service_timing_parameters();
    unsigned calls = queue_calls, reads = fru_calls, writes = led_writes;
    unsigned on = power_on_calls, off = power_off_calls, boot = boot_calls, config = config_set_calls;
    pwr_ctrl_state_t power_state = mock_power_state;
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
    uint32_t cycles = v2_power_sim_cycles();
#endif
    for (size_t kind = 0U; kind < sizeof(commands) / sizeof(commands[0]); ++kind) {
        for (unsigned null_value = 0U; null_value < 2U; ++null_value) {
            cJSON *root = cJSON_Parse(kind == 2U ? SLT_START : START_JSON); CHECK(root);
            if (kind == 1U) {
                CHECK(cJSON_ReplaceItemInObjectCaseSensitive(root, "algorithmType", cJSON_CreateString("parametric")));
                CHECK(cJSON_ReplaceItemInObjectCaseSensitive(root, "algorithmName", cJSON_CreateString("idd_0")));
            }
            if (null_value)
                CHECK(cJSON_ReplaceItemInObjectCaseSensitive(root, "TimingParameters", cJSON_CreateNull()));
            else cJSON_DeleteItemFromObjectCaseSensitive(root, "TimingParameters");
            char *json = cJSON_PrintUnformatted(root); CHECK(json);
            round_command(commands[kind], json, "failed", "invalidArgument");
            cJSON_free(json); cJSON_Delete(root);
            CHECK(!pc_test_service_busy() && !pc_power_service_busy());
            CHECK(queue_calls == calls && fru_calls == reads && led_writes == writes);
            CHECK(power_on_calls == on && power_off_calls == off && boot_calls == boot && config_set_calls == config);
            CHECK(mock_power_state == power_state && pc_test_service_timing_parameters() == saved);
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
            CHECK(v2_power_sim_cycles() == cycles);
#endif
        }
    }
    message("PASS: missing/null timing in FUNC/PARA/SLT rejected without hardware or queue effects\n");
}

/* 模拟UART任务的本地发送完成事件；不伪造RK成功ACK，RK编码另有实际任务回归。 */
static void timing_sent(unsigned count)
{
    pc_dispatch_response_t response;
    for (unsigned i = 0U; i < count; ++i) {
        CHECK(rk_is_timing_command(queued_request.command_id));
        queued_response = (rk_response_t){.cookie = queued_request.cookie, .status = RK_RESPONSE_SENT};
        response_ready = 1;
        CHECK(pc_dispatch_next_response(78U, &response) == 0);
    }
}

/* 输入JSON会在dispatch内释放；校验副本寿命、拒绝不覆盖以及逐分配点失败无副作用。 */
static void timing_storage_check(void)
{
    pc_dispatch_response_t response;
    cJSON *root = cJSON_Parse(START_JSON); CHECK(root);
    cJSON *timing = cJSON_GetObjectItemCaseSensitive(root, "TimingParameters"); CHECK(cJSON_IsObject(timing));
    CHECK(cJSON_AddStringToObject(timing, "Freq", ""));
    CHECK(cJSON_AddStringToObject(timing, "tRCD", ""));
    CHECK(cJSON_AddStringToObject(timing, "CASReadLatency", "20"));
    CHECK(cJSON_AddStringToObject(timing, "CASWriteLatency", "12"));
    char *json = cJSON_PrintUnformatted(root); CHECK(json);
    round_begin();
    timing_required_check();
    unsigned calls = queue_calls;
    round_command(PC_COMMAND_START_TEST, json, "accepted", NULL);
    CHECK(queue_calls == calls + 1U && queued_request.command_id == 0x25U);
    timing_sent(2U); CHECK(queued_request.command_id == 17U);
    const cJSON *saved = pc_test_service_timing_parameters();
    CHECK(saved && saved != timing);
    string_is(saved, "Freq", ""); string_is(saved, "tRCD", "");
    string_is(saved, "CASReadLatency", "20"); string_is(saved, "CASWriteLatency", "12");
    CHECK(!cJSON_GetObjectItemCaseSensitive(saved, "tRC"));
    round_command(PC_COMMAND_START_TEST, START_JSON, "failed", "busy");
    CHECK(pc_test_service_timing_parameters() == saved);
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(78U, &response) == 1 && !pc_test_service_busy());
    CHECK(pc_test_service_timing_parameters() == saved);

    queue_full = 1;
    round_command(PC_COMMAND_START_TEST, json, "failed", "busy");
    queue_full = 0;
    CHECK(pc_test_service_timing_parameters() == saved);
    pc_test_start_view_t start;
    CHECK(pc_test_start_parse(root, &start) == PC_TEST_JSON_OK);
    calls = queue_calls;
    unsigned failure;
    for (failure = 0U; failure < 64U; ++failure) {
        delivery_allocations = 0U; delivery_fail_at = failure;
        timing_allocation_balance = 0;
        pc_json_configure_allocator(timing_allocator, timing_free);
        int result = pc_test_service_start(PC_COMMAND_START_TEST, &start, 17U, &response);
        pc_json_configure_allocator(NULL, NULL);
        if (result == PC_DISPATCH_RESPONSE_READY) break;
        CHECK(result == PC_DISPATCH_ERROR && !pc_test_service_busy());
        CHECK(timing_allocation_balance == 0);
        CHECK(queue_calls == calls && pc_test_service_timing_parameters() == saved);
        string_is(saved, "Freq", "");
    }
    CHECK(failure < 64U && queue_calls == calls + 1U);
    timing_sent(2U); CHECK(queued_request.command_id == 17U);
    cJSON_free(json); cJSON_Delete(root);
    string_is(pc_test_service_timing_parameters(), "Freq", "");
    string_is(pc_test_service_timing_parameters(), "CASReadLatency", "20");
    string_is(pc_test_service_timing_parameters(), "CASWriteLatency", "12");
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(78U, &response) == 1 && !pc_test_service_busy());

    /* 新请求空对象不下发时序，也不继承上一请求；不能由此宣称寄存器恢复默认。 */
    round_command(PC_COMMAND_START_TEST, START_JSON, "accepted", NULL);
    CHECK(cJSON_IsObject(pc_test_service_timing_parameters()) &&
          pc_test_service_timing_parameters()->child == NULL && queued_request.command_id == 17U);
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(78U, &response) == 1 && !pc_test_service_busy());
    /* PARA单独提供写延迟时，只发送本次值，不从上一请求补出读延迟。 */
    root = cJSON_Parse(START_JSON); CHECK(root);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(root, "algorithmType", cJSON_CreateString("parametric")));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(root, "algorithmName", cJSON_CreateString("idd_0")));
    timing = cJSON_GetObjectItemCaseSensitive(root, "TimingParameters"); CHECK(cJSON_IsObject(timing));
    CHECK(cJSON_AddStringToObject(timing, "CASWriteLatency", "8"));
    json = cJSON_PrintUnformatted(root); CHECK(json); cJSON_Delete(root);
    para_command(PC_COMMAND_PARA_START, json, 37U, NULL); cJSON_free(json);
    timing_sent(1U);
    CHECK(cJSON_IsObject(pc_test_service_timing_parameters()) &&
          cJSON_GetArraySize(pc_test_service_timing_parameters()) == 1 && queued_request.command_id == 21U);
    string_is(pc_test_service_timing_parameters(), "CASWriteLatency", "8");
    CHECK(!cJSON_GetObjectItemCaseSensitive(pc_test_service_timing_parameters(), "CASReadLatency"));
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(78U, &response) == 1 && !pc_test_service_busy());
    root = decode(&response, PC_COMMAND_PARA_RESULT, 78U); cJSON_Delete(root);
    round_end();
    message("PASS: timing RAM copy lifetime, partial values, rejection preservation, allocation faults, timing sent before algorithm\n");
}

/* 完整16字段、边界数值、发送失败和中途收尾：PC决定数值，MCU仅转换并有序发送。 */
static void timing_forward_check(void)
{
    static const char *const fields[] = {"CASWriteLatency", "CASReadLatency", "tREFI", "tWR", "tRTP",
        "tRCD", "tRP", "tRASmin", "tRASmax", "tWTR", "tRFCmin", "tFAW", "tCCD", "tRRD", "BL"};
    static const uint16_t commands[] = {0x25,0x26,0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x32,0x33,0x34};
    pc_dispatch_response_t response;
    cJSON *root = cJSON_Parse(START_JSON); CHECK(root);
    cJSON *timing = cJSON_GetObjectItemCaseSensitive(root, "TimingParameters");
    CHECK(cJSON_AddStringToObject(timing, "Freq", ""));
    for (unsigned i = 0U; i < 15U; ++i) {
        char value[16]; snprintf(value, sizeof(value), "%lu", (unsigned long)(i == 1U ? UINT32_MAX : i));
        CHECK(cJSON_AddStringToObject(timing, fields[i], value));
    }
    char *json = cJSON_PrintUnformatted(root); CHECK(json); cJSON_Delete(root);
    round_begin();
    unsigned before = queue_calls;
    round_command(PC_COMMAND_START_TEST, json, "accepted", NULL);
    for (unsigned i = 0U; i < 15U; ++i) {
        CHECK(queued_request.command_id == commands[i]);
        CHECK(queued_request.parameter_value == (i == 1U ? UINT32_MAX : i));
        CHECK(queued_request.timeout_ms == 0U && pc_test_service_busy());
        CHECK(pc_dispatch_next_response(78U, &response) == 0 && queue_calls == before + i + 1U);
        timing_sent(1U);
    }
    CHECK(queued_request.command_id == 17U && queue_calls == before + 16U);
    inject(RK_RESPONSE_COMPLETE, 0U); CHECK(pc_dispatch_next_response(78U, &response) == 1);
    CHECK(!pc_test_service_busy());
    round_command(PC_COMMAND_START_TEST, json, "accepted", NULL);
    before = queue_calls;
    queued_response = (rk_response_t){.cookie = queued_request.cookie, .status = RK_RESPONSE_TX_FAILED};
    response_ready = 1;
    CHECK(pc_dispatch_next_response(78U, &response) == 1 && !pc_test_service_busy());
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 78U);
    string_is(body, "errorCode", "txFailed"); cJSON_Delete(body);
    CHECK(queue_calls == before);
    round_command(PC_COMMAND_START_TEST, json, "accepted", NULL);
#if defined(B1_RK_UART)
    before = queue_calls;
    mock_power_state = PWR_CTRL_STATE_OFF;
    queued_response = (rk_response_t){.cookie = queued_request.cookie, .status = RK_RESPONSE_SENT};
    response_ready = 1;
    CHECK(pc_dispatch_next_response(78U, &response) == 1 && !pc_test_service_busy());
    body = decode(&response, PC_COMMAND_COMMAND_RESP, 78U);
    string_is(body, "errorCode", "powerOff"); cJSON_Delete(body);
    CHECK(queue_calls == before); mock_power_state = PWR_CTRL_STATE_ON;
    round_command(PC_COMMAND_START_TEST, json, "accepted", NULL);
#endif
    before = queue_calls; round_end();
    timing_sent(1U); CHECK(queue_calls == before && !pc_test_service_busy());
    cJSON_free(json);
    round_begin();
    root = cJSON_Parse(START_JSON); timing = cJSON_GetObjectItemCaseSensitive(root, "TimingParameters");
    CHECK(cJSON_AddStringToObject(timing, "Freq", "1600"));
    json = cJSON_PrintUnformatted(root); CHECK(json); cJSON_Delete(root);
    round_command(PC_COMMAND_START_TEST, json, "failed", "invalidState");
    cJSON_free(json); round_end();
    message("PASS: all 15 timing commands preserve uint32 values; no ACK wait; TX failure and discard stop further sends\n");
}

#if defined(PC_POWER_SEQUENCE_TEST)
static void frequency_start_check(void)
{
    pc_dispatch_response_t response;
    power_fixture = 1; configure_power();
    cJSON *root = cJSON_Parse(COLD_START); CHECK(root);
    cJSON *timing = cJSON_GetObjectItemCaseSensitive(root, "TimingParameters");
    CHECK(cJSON_AddStringToObject(timing, "Freq", "1600"));
    CHECK(cJSON_AddStringToObject(timing, "tRCD", "18"));
    char *json = cJSON_PrintUnformatted(root); CHECK(json);
    for (unsigned run = 0U; run < 3U; ++run) {
        if (run == 2U) {
            cJSON_free(json);
            CHECK(cJSON_ReplaceItemInObjectCaseSensitive(timing, "Freq", cJSON_CreateString("1800")));
            json = cJSON_PrintUnformatted(root); CHECK(json);
        }
        unsigned on = power_on_calls, before = queue_calls;
        start_expect(json, NULL);
        uint32_t token;
        if (run != 1U) {
            CHECK(rk_link_frequency_get(&token) == (run == 2U ? 1800 : 1600));
            CHECK(token && rk_link_frequency_status(token) == 0 && queue_calls == before);
            if (run == 2U) direct_off_to_hold();
            /* 在进入ON/普通心跳前完成频率发送事件，证明不会等SPL才回复。 */
            rk_link_frequency_note_tx(token, 1);
            cold_to_ready(); confirm_power(1); step_power(0U);
        } else {
            CHECK(pc_power_service_diagnostic()->phase == PC_POWER_DONE && power_on_calls == on);
        }
        CHECK(pc_dispatch_next_response(105U, &response) == 0);
        CHECK(queued_request.command_id == 0x2BU && queued_request.parameter_value == 18U);
        queued_response = (rk_response_t){.cookie = queued_request.cookie, .status = RK_RESPONSE_SENT};
        response_ready = 1;
        CHECK(pc_dispatch_next_response(105U, &response) == 1);
        cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 105U);
        string_is(body, "status", "completed"); cJSON_Delete(body);
        CHECK(queued_request.command_id == 17U && queue_calls == before + 2U);
        CHECK(power_on_calls == on + (run == 1U ? 0U : 1U));
        result_is(0U, NULL, "SITE-7");
    }
    /* 非默认频率未得到本轮查询时，不以普通心跳冒充已经回复频率。 */
    configure_power(); start_expect(json, NULL); cold_to_ready(); confirm_power(1); step_power(0U);
    unsigned before = queue_calls;
    CHECK(pc_dispatch_next_response(105U, &response) == 0);
    mock_tick += 60000U;
    CHECK(pc_dispatch_next_response(105U, &response) == 1 && queue_calls == before);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 105U);
    string_is(body, "errorCode", "timeout"); cJSON_Delete(body);
    CHECK(!pc_test_service_busy());
    start_expect(json, NULL);
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_REQUEST_OFF);
    uint32_t token; (void)rk_link_frequency_get(&token); rk_link_frequency_note_tx(token, 0);
    direct_off_to_hold(); cold_to_ready(); confirm_power(1); step_power(0U);
    CHECK(pc_dispatch_next_response(105U, &response) == 1);
    body = decode(&response, PC_COMMAND_COMMAND_RESP, 105U);
    string_is(body, "errorCode", "txFailed"); cJSON_Delete(body);
    CHECK(!pc_test_service_busy() && queue_calls == before);
    cJSON_free(json); cJSON_Delete(root); round_end();
    message("PASS: startup frequency cache before power, unchanged frequency reuses boot, changed frequency cold-starts, missed query/TX failure gate algorithms\n");
}
#endif

#if defined(PC_POWER_SEQUENCE_TEST) || (defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION)
static void slt_timing_check(void)
{
    pc_dispatch_response_t response;
#if defined(PC_POWER_SEQUENCE_TEST)
    power_fixture = 1; configure_power();
#endif
    round_begin();
    cJSON *root = cJSON_Parse(SLT_START); CHECK(root);
    cJSON *timing = cJSON_GetObjectItemCaseSensitive(root, "TimingParameters");
    CHECK(cJSON_AddStringToObject(timing, "Freq", "1600"));
    CHECK(cJSON_AddStringToObject(timing, "tRASmin", "21"));
    CHECK(cJSON_AddStringToObject(timing, "tRASmax", "42"));
    char *json = cJSON_PrintUnformatted(root); CHECK(json); cJSON_Delete(root);
    unsigned before = queue_calls;
    round_command(PC_COMMAND_SLT_START, json, "accepted", NULL); cJSON_free(json);
    for (unsigned i = 0U; i < 12U; ++i) {
        mock_tick += 20U;
#if defined(PC_POWER_SEQUENCE_TEST)
        if (mock_power_state == PWR_CTRL_STATE_WAIT_FIRST_RAIL) mock_power_state = PWR_CTRL_STATE_ON;
        if (mock_power_state == PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF) mock_power_state = PWR_CTRL_STATE_OFF;
#endif
        pc_dispatch_poll();
        CHECK(pc_dispatch_next_response(81U, &response) == 0 && queue_calls == before);
    }
    uint32_t token;
    CHECK(rk_link_frequency_get(&token) == 1600);
    rk_link_frequency_note_tx(token, 1);
    CHECK(pc_dispatch_next_response(81U, &response) == 0 && queue_calls == before);
    tlv_frame_t heartbeat = {.sequence = rk_link_next_sequence()};
    rk_link_ready_note_tx(heartbeat.sequence); rk_link_ready_note_rx(&heartbeat);
    CHECK(pc_dispatch_next_response(81U, &response) == 0);
    CHECK(queued_request.command_id == 0x2DU && queued_request.parameter_value == 21U);
    timing_sent(1U);
    CHECK(queued_request.command_id == 0x2EU && queued_request.parameter_value == 42U);
    queued_response = (rk_response_t){.cookie = queued_request.cookie, .status = RK_RESPONSE_SENT};
    response_ready = 1;
    CHECK(pc_dispatch_next_response(81U, &response) == 1);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 81U);
    string_is(body, "status", "completed"); cJSON_Delete(body);
    CHECK(queue_calls == before + 2U && pc_test_service_busy());
    round_command(PC_COMMAND_SLT_RESULT, SLT_RESULT, "completed", NULL);
    CHECK(!pc_test_service_busy()); round_end();
    message("PASS: SLT with parameters waits frequency/SPL then sends timing only; PC 010B owns completion\n");
}
#endif

#if defined(PC_POWER_SEQUENCE_TEST) || (defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION)
/* 推进SLT电源；没有注入任何RK心跳，仍须只按本地电源完成返回completed。 */
static void slt_complete(int discarded)
{
    pc_dispatch_response_t response;
    unsigned queued = queue_calls;
    int delivered = 0;
    for (unsigned i = 0U; i < 12U; ++i) {
        mock_tick += 20U;
#if defined(PC_POWER_SEQUENCE_TEST)
        if (mock_power_state == PWR_CTRL_STATE_WAIT_FIRST_RAIL) mock_power_state = PWR_CTRL_STATE_ON;
        if (mock_power_state == PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF) mock_power_state = PWR_CTRL_STATE_OFF;
#endif
        pc_dispatch_poll();
        CHECK(pc_power_service_diagnostic()->phase != PC_POWER_WAIT_RK_READY);
        int ready = pc_dispatch_next_response(81U, &response);
        CHECK(ready >= 0);
        if (ready) {
            CHECK(!discarded && !delivered);
            cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 81U);
            number_is(body, "requestCommandId", PC_COMMAND_SLT_START);
            CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(body, "algorithmIndex")));
            string_is(body, "status", "completed"); cJSON_Delete(body);
            delivered = 1;
        }
    }
    CHECK(delivered == !discarded && !pc_power_service_busy() && pc_test_service_busy());
    CHECK(queue_calls == queued);
}

static void mutated_slt(uint16_t command, const char *source, const char *field,
                        cJSON *value, const char *error)
{
    cJSON *body = cJSON_Parse(source); CHECK(body && value);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(body, field, value));
    char *json = cJSON_PrintUnformatted(body); CHECK(json);
    round_command(command, json, "failed", error);
    cJSON_free(json); cJSON_Delete(body);
}

static void slt_flow_check(void)
{
    pc_dispatch_response_t response;
#if defined(PC_POWER_SEQUENCE_TEST)
    power_fixture = 1;
    configure_power();
    CHECK(pc_power_service_configure(&sequence_config, NULL) == 0);
    unsigned arms = rk_arms;
#endif
    round_begin();
    mutated_slt(PC_COMMAND_SLT_START, SLT_START, "algorithmName", cJSON_CreateString("MemTest86"), "invalidArgument");
    mutated_slt(PC_COMMAND_SLT_START, SLT_START, "algorithmType", cJSON_CreateString("functional"), "invalidArgument");
    mutated_slt(PC_COMMAND_SLT_START, SLT_START, "algorithmIndex", cJSON_CreateNumber(0), "invalidArgument");
    mutated_slt(PC_COMMAND_SLT_START, SLT_START, "power", cJSON_CreateNull(), "invalidArgument");
    mutated_slt(PC_COMMAND_SLT_START, SLT_START, "bootMode", cJSON_CreateString(""), "invalidArgument");
    mutated_slt(PC_COMMAND_SLT_START, SLT_START, "reboot", cJSON_CreateString("true"), "invalidArgument");
    CHECK(!pc_test_service_busy());
    round_command(PC_COMMAND_SLT_RESULT, SLT_RESULT, "failed", "invalidState");
    cJSON *timed = cJSON_Parse(SLT_START); CHECK(timed);
    cJSON *timing = cJSON_GetObjectItemCaseSensitive(timed, "TimingParameters"); CHECK(cJSON_IsObject(timing));
    CHECK(cJSON_AddStringToObject(timing, "CASReadLatency", ""));
    CHECK(cJSON_AddStringToObject(timing, "CASWriteLatency", ""));
    pc_test_start_view_t timed_start;
    CHECK(pc_slt_start_parse(timed, &timed_start) == PC_TEST_JSON_OK);
    unsigned previous_on = power_on_calls, previous_off = power_off_calls, previous_boot = boot_calls;
    const cJSON *previous_timing = pc_test_service_timing_parameters();
    unsigned failure;
    for (failure = 0U; failure < 64U; ++failure) {
        delivery_allocations = 0U; delivery_fail_at = failure; timing_allocation_balance = 0;
        pc_json_configure_allocator(timing_allocator, timing_free);
        int result = pc_test_slt_start(&timed_start, 77U, &response);
        pc_json_configure_allocator(NULL, NULL);
        if (result == PC_DISPATCH_RESPONSE_READY) break;
        CHECK(result == PC_DISPATCH_ERROR && timing_allocation_balance == 0);
        CHECK(!pc_test_service_busy() && !pc_power_service_busy());
        CHECK(power_on_calls == previous_on && power_off_calls == previous_off && boot_calls == previous_boot);
        CHECK(pc_test_service_timing_parameters() == previous_timing);
    }
    CHECK(failure < 64U);
    cJSON_Delete(timed);
    string_is(pc_test_service_timing_parameters(), "CASReadLatency", "");
    string_is(pc_test_service_timing_parameters(), "CASWriteLatency", "");
    CHECK(!cJSON_GetObjectItemCaseSensitive(pc_test_service_timing_parameters(), "Freq"));
    round_command(PC_COMMAND_SLT_RESULT, SLT_RESULT, "failed", "invalidState");
    slt_complete(0);
#if defined(PC_POWER_SEQUENCE_TEST)
    CHECK(rk_arms == arms);
#endif
    /* 串口完全不回包也不结束SLT；等待PC回送，不能占用期间再启动FUNC或升级。 */
    mock_tick += 3600000U; pc_dispatch_poll();
    CHECK(pc_dispatch_next_response(82U, &response) == 0 && pc_test_service_busy());
    round_command(PC_COMMAND_START_TEST, START_JSON, "failed", "busy");
    round_command(PC_COMMAND_SLT_START, SLT_START, "failed", "busy");
    round_command(PC_COMMAND_POWER_OFF, "{}", "failed", "busy");
    CHECK(dispatch(PC_COMMAND_QUERY_POWER_MONITOR, "{}", 82U, &response) == 1);
    mutated_slt(PC_COMMAND_SLT_RESULT, SLT_RESULT, "algorithmName", cJSON_CreateString("old"), "invalidState");
    mutated_slt(PC_COMMAND_SLT_RESULT, SLT_RESULT, "siteId", cJSON_CreateString("SITE-8"), "invalidState");
    mutated_slt(PC_COMMAND_SLT_RESULT, SLT_RESULT, "algorithmIndex", cJSON_CreateNumber(1), "invalidArgument");
    mutated_slt(PC_COMMAND_SLT_RESULT, SLT_RESULT, "measurementData", cJSON_CreateObject(), "invalidArgument");
    mutated_slt(PC_COMMAND_SLT_RESULT, SLT_RESULT, "testTimeMs", cJSON_CreateNumber(-1), "invalidArgument");
    mutated_slt(PC_COMMAND_SLT_RESULT, SLT_RESULT, "testResults", cJSON_Parse("[{\"mirrorIndex\":0,\"testStatus\":0,\"resultDesc\":null,\"physicalAddr\":\"\"},{\"mirrorIndex\":0,\"testStatus\":1,\"resultDesc\":null,\"physicalAddr\":\"\"}]"), "invalidArgument");
    CHECK(pc_test_service_busy());
    round_command(PC_COMMAND_SLT_RESULT, SLT_RESULT, "completed", NULL);
    CHECK(!pc_test_service_busy());
    round_command(PC_COMMAND_SLT_RESULT, SLT_RESULT, "failed", "invalidState");
    /* 每次新SLT都须重新启动；true/false均只执行一次，忙时重复请求不得再启动。 */
    for (unsigned reboot = 0U; reboot < 2U; ++reboot) {
#if defined(PC_POWER_SEQUENCE_TEST)
        unsigned previous_on = power_on_calls, previous_off = power_off_calls, previous_boot = boot_calls;
#else
        uint32_t previous_cycles = v2_power_sim_cycles();
#endif
        cJSON *start = cJSON_Parse(SLT_START); CHECK(start);
        CHECK(cJSON_ReplaceItemInObjectCaseSensitive(start, "reboot", cJSON_CreateBool(reboot)));
        char *json = cJSON_PrintUnformatted(start); CHECK(json);
        round_command(PC_COMMAND_SLT_START, json, "accepted", NULL);
        CHECK(cJSON_IsObject(pc_test_service_timing_parameters()) &&
              pc_test_service_timing_parameters()->child == NULL);
        round_command(PC_COMMAND_SLT_START, json, "failed", "busy");
        cJSON_free(json); cJSON_Delete(start);
        slt_complete(0);
#if defined(PC_POWER_SEQUENCE_TEST)
        CHECK(power_on_calls == previous_on + 1U && power_off_calls == previous_off + 1U);
        CHECK(boot_calls == previous_boot + 1U && rk_arms == arms);
#else
        CHECK(v2_power_sim_cycles() == previous_cycles + 1U);
#endif
        round_command(PC_COMMAND_SLT_RESULT, SLT_RESULT, "completed", NULL);
        CHECK(!pc_test_service_busy());
    }
    /* 0107提前结束不伪造SLT结束，也不取消已受理的启动；迟到010B仅回收占用。 */
#if defined(PC_POWER_SEQUENCE_TEST)
    unsigned on = power_on_calls, off = power_off_calls, boot = boot_calls;
#else
    uint32_t cycles = v2_power_sim_cycles();
#endif
    round_command(PC_COMMAND_SLT_START, SLT_START, "accepted", NULL);
    round_end(); unsigned writes = led_writes;
    slt_complete(1);
#if defined(PC_POWER_SEQUENCE_TEST)
    CHECK(power_on_calls == on + 1U && power_off_calls == off + 1U && boot_calls == boot + 1U);
#else
    CHECK(v2_power_sim_cycles() == cycles + 1U);
#endif
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "failed", "busy");
    round_command(PC_COMMAND_SLT_RESULT, SLT_RESULT, "completed", NULL);
    CHECK(!pc_test_service_busy() && led_writes == writes);
    round_begin();
    /* 硬件异常仍报告失败，不因“无需检查RK”而吞掉驱动错误。 */
#if defined(PC_POWER_SEQUENCE_TEST)
    /* 同一电压/Boot的SLT准备完成不能冒充FUNC已ready，仍需FUNC适配器。 */
    round_command(PC_COMMAND_START_TEST, COLD_START, "failed", "notReady");
    CHECK(!pc_test_service_busy() && rk_arms == arms);
    configure_power();
    unsigned queued = queue_calls;
    round_command(PC_COMMAND_START_TEST, COLD_START, "accepted", NULL);
    round_end(); writes = led_writes;
    cold_to_ready(); confirm_power(1); step_power(0U);
    CHECK(pc_dispatch_next_response(83U, &response) == 0);
    CHECK(!pc_test_service_busy() && !pc_power_service_busy() && queue_calls == queued && led_writes == writes);
    round_begin();
    configure_power(); boot_failure = 1;
    round_command(PC_COMMAND_SLT_START, SLT_START, "accepted", NULL);
    step_power(10U);
    CHECK(pc_dispatch_next_response(83U, &response) == 1);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 83U);
    string_is(body, "errorCode", "executionFailed"); cJSON_Delete(body);
    CHECK(!pc_test_service_busy());
    configure_power();
    power_failure = PWR_CTRL_ERR_FAULT;
    round_command(PC_COMMAND_SLT_START, SLT_START, "accepted", NULL);
    step_power(10U); step_power(5U);
    CHECK(pc_dispatch_next_response(84U, &response) == 1);
    body = decode(&response, PC_COMMAND_COMMAND_RESP, 84U);
    string_is(body, "errorCode", "executionFailed"); cJSON_Delete(body);
    CHECK(!pc_test_service_busy() && queue_calls == queued);
    configure_power();
#endif
    round_command(PC_COMMAND_SLT_START, SLT_START, "accepted", NULL); slt_complete(0);
    /* 断线不认领旧SLT，无实例ID时不能用新连接的同名报告冒充本次结果。 */
    pc_dispatch_disconnected();
    round_command(PC_COMMAND_SLT_RESULT, SLT_RESULT, "failed", "invalidState");
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "failed", "busy");
    CHECK(pc_test_service_busy());
    message("PASS: PC RK service SLT always cold-starts even with same target/reboot=false; busy does not restart; no RK ready; PC completion and ownership\n");
}
#endif
#endif

void test_main(void)
{
#if defined(PC_TEST_ROUND_TEST)
    round_flow_check();
    para_flow_check();
    timing_storage_check();
    timing_forward_check();
#if defined(PC_POWER_SEQUENCE_TEST)
    frequency_start_check();
#endif
#if defined(PC_POWER_SEQUENCE_TEST) || (defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION)
    slt_timing_check();
    slt_flow_check();
#endif
    finish(0U);
#endif
#if defined(PC_POWER_SEQUENCE_TEST)
    power_sequence_check();
#if defined(TEST_B1_CONSOLE_POWER)
    console_power_check();
#endif
    finish(0U);
#endif
#if defined(ENABLE_V2_MATS_FAIL_TEST) && ENABLE_V2_MATS_FAIL_TEST
    mats_fail_scope_check();
    delivery_retry_check();
    finish(0U);
#endif
    pc_dispatch_response_t response;
    CHECK(!pc_test_service_busy());
#if !defined(ENABLE_V2_POWER_SIMULATION) || !ENABLE_V2_POWER_SIMULATION
    start_expect("{\"algorithmType\":\"functional\",\"algorithmName\":\"mats\",\"algorithmIndex\":37,\"power\":{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1800\",\"vdd2\":\"1100\",\"vddq\":\"600\"},\"reboot\":true,\"bootMode\":\"emmc\",\"TimingParameters\":{}}", "notConfigured");
    fru_calls = 0U;
#endif
#if defined(B1_RK_UART)
    /* 真实ON是额外门禁；OFF、故障和上电中均不投递RK，不启动统计。 */
    const pwr_ctrl_state_t denied[] = {PWR_CTRL_STATE_OFF, PWR_CTRL_STATE_FAULT,
        PWR_CTRL_STATE_WAIT_FIRST_RAIL, PWR_CTRL_STATE_WAIT_SECOND_RAIL};
    for (unsigned i = 0; i < sizeof(denied) / sizeof(denied[0]); ++i) {
        mock_power_state = denied[i];
        start_expect(START_JSON, "notReady");
        CHECK(fru_calls == 0U && queue_calls == 0U && !pc_test_service_busy());
        CHECK(measured_stats.state == IDD_STATS_IDLE);
    }
    mock_power_state = PWR_CTRL_STATE_ON;
    measurement_start_error = IF_TIMEOUT;
    start_expect(START_JSON, "busy");
    measurement_start_error = 0;
    fru_calls = 0U;
#endif
    start_expect("{\"algorithmType\":\"functional\",\"algorithmName\":\"unknown\",\"algorithmIndex\":37,\"power\":null,\"reboot\":false,\"bootMode\":null,\"TimingParameters\":{}}", "notSupported");
    start_expect("{\"algorithmType\":\"parametric\",\"algorithmName\":\"idd_0\",\"algorithmIndex\":37,\"power\":null,\"reboot\":false,\"bootMode\":null,\"TimingParameters\":{}}", "notSupported");
    fru_error = 1;
    start_expect(START_JSON, "flashError");
    CHECK(queue_calls == 0U && !pc_test_service_busy());
    fru_error = 0;
    rkRequestQueue = NULL;
    start_expect(START_JSON, "notReady");
    rkRequestQueue = (void *)1;
    queue_full = 1;
    start_expect(START_JSON, "busy");
    CHECK(!pc_test_service_busy());
    queue_full = 0;

    /* 响应内存不足不能先执行RK；直接服务入口复用已经解析好的视图。 */
    cJSON *root = cJSON_Parse(START_JSON);
    pc_test_start_view_t view;
    CHECK(pc_test_start_parse(root, &view) == PC_TEST_JSON_OK);
    unsigned before = queue_calls;
    pc_json_configure_allocator(no_memory, free);
    CHECK(pc_test_service_start(PC_COMMAND_START_TEST, &view, 17U, &response) == PC_DISPATCH_ERROR);
    CHECK(queue_calls == before && !pc_test_service_busy());
    pc_json_configure_allocator(NULL, NULL);
    cJSON_Delete(root);

    start_expect(START_JSON, NULL);
    CHECK(pc_test_service_busy() && queued_request.command_id == 17U && queued_request.timeout_ms == 0U);
    uint32_t first_cookie = queued_request.cookie;
    before = queue_calls;
    start_expect(START_JSON, "busy");
    CHECK(queue_calls == before);
    CHECK(pc_dispatch_next_response(18U, &response) == 0);

    /* 在途时电源/Boot修改拒绝；心跳可用，历史升级编号始终不支持。 */
    const uint16_t commands[] = {PC_COMMAND_POWER_ON, PC_COMMAND_POWER_OFF, PC_COMMAND_RESET_RK,
        PC_COMMAND_SET_BOOT_MODE, PC_COMMAND_SET_DDR_CONFIG};
    for (unsigned i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        CHECK(dispatch(commands[i], "{}", 18U, &response) == PC_DISPATCH_RESPONSE_READY);
        cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 18U);
        string_is(body, "status", "failed"); string_is(body, "errorCode", "busy");
        number_is(body, "requestCommandId", commands[i]); cJSON_Delete(body);
    }
    const char *writes[] = {"mcu_begin", "mcu_write", "mcu_finish", "mcu_cancel", "rk_begin"};
    for (unsigned i = 0U; i < sizeof(writes) / sizeof(writes[0]); ++i) {
        root = cJSON_CreateObject(); CHECK(cJSON_AddStringToObject(root, "action", writes[i]));
        char *json = cJSON_PrintUnformatted(root); CHECK(json);
        CHECK(dispatch(PC_COMMAND_FIRMWARE_UPGRADE_REQUEST, json, 18U, &response) == PC_DISPATCH_RESPONSE_READY);
        cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 18U);
        string_is(body, "errorCode", "notSupported"); cJSON_Delete(body); cJSON_free(json); cJSON_Delete(root);
    }
    CHECK(dispatch(PC_COMMAND_CLIENT_HEARTBEAT, "{}", 18U, &response) == PC_DISPATCH_RESPONSE_READY);
    root = decode(&response, PC_COMMAND_SERVER_HEARTBEAT, 18U); CHECK(root->child == NULL); cJSON_Delete(root);
    CHECK(dispatch(PC_COMMAND_FIRMWARE_UPGRADE_REQUEST, "{\"action\":\"mcu_query\"}", 18U, &response) == PC_DISPATCH_RESPONSE_READY);
    root = decode(&response, PC_COMMAND_COMMAND_RESP, 18U);
    string_is(root, "errorCode", "notSupported"); cJSON_Delete(root);
    CHECK(pc_test_service_busy());

    /* 错误cookie、无连接轮询均不能偷走当前连接的合法结果。 */
    inject(RK_RESPONSE_COMPLETE, 0U); queued_response.cookie++;
    CHECK(pc_dispatch_next_response(18U, &response) == 0 && pc_test_service_busy());
    inject(RK_RESPONSE_COMPLETE, 0U);
    pc_dispatch_poll(); CHECK(response_ready && pc_test_service_busy());
    response_ready = 0;
    result_is(0U, NULL, "SITE-7");
    /* 真结果路径含MATS：显式零地址PASS输出null，反序非零失败明细按die归位。 */
    for (uint8_t mask = 0U; mask < 4U; ++mask) {
        start_expect(START_JSON, NULL);
        inject_mask(RK_RESPONSE_COMPLETE, mask, 1U, UINT64_MAX);
        CHECK(pc_dispatch_next_response(19U, &response) == 1);
        root = decode(&response, PC_COMMAND_TEST_RESULT_REPORT, 19U);
        const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "testResults");
        CHECK(cJSON_GetArraySize(results) == 2);
        for (unsigned die = 0U; die < 2U; ++die) {
            const cJSON *chip = cJSON_GetArrayItem(results, die);
            uint8_t failed = (mask & (1U << die)) != 0U;
            number_is(chip, "mirrorIndex", die);
            number_is(chip, "testStatus", failed ? 0U : 1U);
            const cJSON *desc = cJSON_GetObjectItemCaseSensitive(chip, "resultDesc");
            string_is(desc, "errorType", "");
            if (failed) string_is(desc, "firstErrorAddress", die ? "0xFFFFFFFFFFFFFFFF" : "0x0000000000000001");
            else CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(desc, "firstErrorAddress")));
            string_is(chip, "physicalAddr", "");
        }
        cJSON_Delete(root);
        CHECK(!pc_test_service_busy());
    }
#if defined(B1_RK_UART)
    b1_measurement_check();
#endif
    start_expect(START_JSON, NULL);
    CHECK(queued_request.cookie != first_cookie);
    result_is(UINT64_C(0x123456789ABCDEF0), "0x123456789ABCDEF0", "SITE-7");
    b2_site = 8U;
    start_expect(START_JSON, NULL);
    result_is(0U, NULL, "8");
    b2_site = 0U;
    memset(site_label, 0, sizeof(site_label));
    start_expect(START_JSON, NULL);
    result_is(UINT64_MAX, "0xFFFFFFFFFFFFFFFF", "");

    /* 断开PC仅解除结果归属；新连接不能接旧结果，也不能重复启动RK。 */
    start_expect(START_JSON, NULL);
    pc_dispatch_disconnected(); CHECK(pc_test_service_busy());
    start_expect(START_JSON, "busy");
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(1U, &response) == 0 && !pc_test_service_busy());
    start_expect(START_JSON, NULL); pc_dispatch_disconnected();
    inject(RK_RESPONSE_COMPLETE, 0U); pc_dispatch_poll(); CHECK(!pc_test_service_busy());

    /* 显式有限等待超时不是芯片FAIL；只有RECOVERED解除未知执行状态。 */
    start_expect("{\"algorithmType\":\"functional\",\"algorithmName\":\"mats\",\"algorithmIndex\":37,\"power\":null,\"reboot\":false,\"bootMode\":null,\"timeoutMs\":1200000,\"TimingParameters\":{}}", NULL);
    CHECK(queued_request.timeout_ms == 1200000U);
    inject(RK_RESPONSE_TIMEOUT, 0U);
    CHECK(pc_dispatch_next_response(19U, &response) == 1); status_is(&response, "timeout", 19U);
    CHECK(pc_test_service_busy() && pc_dispatch_next_response(20U, &response) == 0);
    start_expect(START_JSON, "busy");
    pc_dispatch_disconnected(); start_expect(START_JSON, "busy");
    inject(RK_RESPONSE_RECOVERED, 0U);
    CHECK(pc_dispatch_next_response(1U, &response) == 0 && !pc_test_service_busy());
    const rk_response_status_t errors[] = {RK_RESPONSE_TX_FAILED, RK_RESPONSE_INVALID, RK_RESPONSE_POWER_OFF};
    const char *names[] = {"txFailed", "invalidArgument", "powerOff"};
    for (unsigned i = 0U; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        start_expect(START_JSON, NULL); inject(errors[i], UINT64_MAX);
        CHECK(pc_dispatch_next_response(21U, &response) == 1); status_is(&response, names[i], 21U);
        CHECK(!pc_test_service_busy());
    }
    message("PASS: PC RK service actual dispatch/queue/JSON, PASS and 64-bit FAIL address\n");
    message("PASS: guards, disconnect ownership, timeout recovery and independent Seq\n");
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
    power_simulation_check();
#endif
    delivery_retry_check();
    finish(0U);
}
