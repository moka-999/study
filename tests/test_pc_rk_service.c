/* QEMU执行正式PC分发、RK业务及命令表；平台队列/FRU/OTA状态由本文件控制。 */
#include "pc_test_service.h"
#include "pc_tester_info.h"
#include "pc_ota.h"
#include "monitor_task.h"
#include "result_led.h"
#include "ota_app.h"
#include "rk_link.h"
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
#include "v2_power_sim.h"
#endif
#if defined(B1_RK_UART) || (defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION)
static uint32_t mock_tick = 1000U;
#endif
#if defined(B1_RK_UART)
static pwr_ctrl_state_t mock_power_state = PWR_CTRL_STATE_OFF;
static idd_stats_t measured_stats;
static monitor_snapshot_t measured_latest;
static int measurement_start_error;
/* 只替代采样任务的端口，不替代统计算法；下面显式注入的测量值不是硬件实测。 */
int monitor_idd_start(uint32_t duration, uint32_t minimum)
{
    if (measurement_start_error) return measurement_start_error;
    return idd_stats_begin(&measured_stats, mock_tick, duration, minimum);
}
void monitor_idd_cancel(void) { idd_stats_cancel(&measured_stats); }
void monitor_idd_finish(void) { idd_stats_finish(&measured_stats); }
int monitor_idd_get(idd_stats_t *out)
{
    if (!out) return 0;
    idd_stats_poll(&measured_stats, mock_tick);
    *out = measured_stats;
    return 1;
}
#endif
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define S1(x) #x
#define S(x) S1(x)
#define CHECK(c) do { if (!(c)) fail("FAIL " S(__LINE__) ": " #c "\n"); } while (0)
#define START_JSON "{\"algorithmType\":\"functional\",\"algorithmName\":\"mats\",\"algorithmIndex\":37,\"power\":null,\"reboot\":false,\"bootMode\":null}"
static int confirmed = 1, fru_error, queue_full, response_ready;
static unsigned queue_calls, fru_calls, ota_calls;
static ota_download_state_t download_state;
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

int ota_app_is_confirmed(void) { return confirmed; }
void ota_download_get_progress(ota_download_progress_t *out)
{ memset(out, 0, sizeof(*out)); out->state = download_state; }
void ota_download_poll(void) { }
void ota_download_disconnected(void) { }
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
#if defined(B1_RK_UART) || (defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION)
    return mock_tick;
#else
    fail("FAIL unexpected HAL_GetTick\n");
#endif
}
pwr_ctrl_state_t pwr_ctrl_get_state(void) {
#if defined(B1_RK_UART)
    return mock_power_state;
#else
    fail("FAIL unexpected power access\n");
#endif
}
pwr_ctrl_result_t monitor_request_power_on(uint32_t now)
{ (void)now; fail("FAIL power on\n"); }
monitor_boot_result_t monitor_set_boot_mode(boot_mode_t mode)
{ (void)mode; fail("FAIL Boot write\n"); }
monitor_boot_result_t monitor_get_boot_mode(boot_mode_t *mode, uint16_t *mv)
{ (void)mode; (void)mv; fail("FAIL Boot query\n"); }
pwr_ctrl_result_t monitor_apply_ddr_config(ddr_memory_type_t type, const int16_t steps[3])
{ (void)type; (void)steps; fail("FAIL DDR write\n"); }
int monitor_get_latest(monitor_snapshot_t *out)
{
#if defined(B1_RK_UART)
    *out = measured_latest;
    return 1;
#else
    (void)out; fail("FAIL monitor access\n");
#endif
}
HAL_StatusTypeDef result_led_set(result_led_target_t target, result_led_state_t state)
{ (void)target; (void)state; fail("FAIL LED write\n"); }
HAL_StatusTypeDef result_led_get_all(result_led_state_t states[3])
{ (void)states; fail("FAIL LED query\n"); }
pc_dispatch_result_t pc_ota_handle(const pc_protocol_frame_t *request, uint32_t sequence,
                                  const cJSON *root, pc_dispatch_response_t *response)
{
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    CHECK(request->command_id == PC_COMMAND_FIRMWARE_UPGRADE_REQUEST);
    CHECK(cJSON_IsString(action) && strcmp(action->valuestring, "mcu_query") == 0);
    ++ota_calls;
    CHECK(pc_protocol_build_response(PC_COMMAND_FIRMWARE_UPGRADE_RESP, sequence,
        (const uint8_t *)"{}", 2U, response->data, sizeof(response->data), &response->length) == 0);
    return PC_DISPATCH_RESPONSE_READY;
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
    unsigned before = queue_calls;
    CHECK(dispatch(PC_COMMAND_START_TEST, json, 17U, &response) == PC_DISPATCH_RESPONSE_READY);
    status_is(&response, error, 17U);
    if (error && strcmp(error, "busy") != 0) CHECK(queue_calls == before);
}
static void inject(rk_response_status_t status, uint64_t address)
{
    CHECK(!response_ready);
    memset(&queued_response, 0, sizeof(queued_response));
    queued_response.cookie = queued_request.cookie;
    queued_response.sequence = UINT64_C(0xAABBCCDDEEFF0011);
    queued_response.status = status;
    queued_response.result.error_address = address;
    queued_response.result.elapsed_ms = UINT64_C(42000);
    queued_response.result.command_id = queued_request.command_id;
    queued_response.result.die = 9U;
    response_ready = 1;
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
    CHECK(cJSON_IsArray(results) && cJSON_GetArraySize(results) == 1);
    const cJSON *chip = cJSON_GetArrayItem(results, 0);
    /* JSON仍按键名解释；这里只核对本端输出顺序与协议示例一致，兼顾PASS和FAIL。 */
    static const char *const result_fields[] = {
        "siteId", "mirrorIndex", "testStatus", "resultDesc", "physicalAddr", "measurementData"
    };
    const cJSON *field = chip->child;
    for (size_t i = 0U; i < sizeof(result_fields) / sizeof(result_fields[0]); ++i) {
        CHECK(field && field->string && strcmp(field->string, result_fields[i]) == 0);
        field = field->next;
    }
    CHECK(field == NULL);
    string_is(chip, "siteId", expected_site);
    string_is(chip, "physicalAddr", "");
    number_is(chip, "mirrorIndex", 0U);
    number_is(chip, "testStatus", address == 0U ? 1U : 2U);
    const cJSON *desc = cJSON_GetObjectItemCaseSensitive(chip, "resultDesc");
    CHECK(cJSON_IsObject(desc));
    if (expected_address) string_is(desc, "firstErrorAddress", expected_address);
    else CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(desc, "firstErrorAddress")));
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(desc, "errorType")));
#if defined(B1_RK_UART) || (defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION)
    const cJSON *measurement = cJSON_GetObjectItemCaseSensitive(chip, "measurementData");
    CHECK(cJSON_IsObject(measurement) && cJSON_GetArraySize(measurement) == 18);
#else
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(chip, "measurementData")));
#endif
    cJSON_Delete(body);
    CHECK(!pc_test_service_busy());
    CHECK(pc_dispatch_next_response(19U, &response) == 0);
}
static void *no_memory(size_t bytes) { (void)bytes; return NULL; }

#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
#define POWER_START "{\"algorithmType\":\"functional\",\"algorithmName\":\"mats\",\"algorithmIndex\":37,\"power\":{\"memoryType\":\"LPDDR4X\",\"vdd1Steps\":0,\"vdd2Steps\":0,\"vddqSteps\":0},\"reboot\":false,\"bootMode\":\"emmc\"}"
#define CHANGED_START "{\"algorithmType\":\"functional\",\"algorithmName\":\"mats\",\"algorithmIndex\":37,\"power\":{\"memoryType\":\"LPDDR4X\",\"vdd1Steps\":1,\"vdd2Steps\":0,\"vddqSteps\":0},\"reboot\":false,\"bootMode\":\"emmc\"}"
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
    control(PC_COMMAND_SET_DDR_CONFIG, "{\"memoryType\":\"LPDDR4X\",\"vdd1Steps\":1,\"vdd2Steps\":0,\"vddqSteps\":0}", "failed", "invalidState");
    control(PC_COMMAND_SET_DDR_CONFIG, "{\"memoryType\":\"LPDDR4X\",\"vdd1Steps\":0,\"vdd2Steps\":0,\"vddqSteps\":0}", "completed", NULL);
    control(PC_COMMAND_POWER_ON, "{}", "accepted", NULL);
    control(PC_COMMAND_SET_BOOT_MODE, boot_json[0], "failed", "invalidState");
    control(PC_COMMAND_RESET_RK, "{}", "completed", NULL);
    control(PC_COMMAND_SET_DDR_CONFIG, "{\"memoryType\":\"LPDDR4X\",\"vdd1Steps\":1,\"vdd2Steps\":-1,\"vddqSteps\":2}", "completed", NULL);
    control(PC_COMMAND_SET_DDR_CONFIG, "{\"memoryType\":\"LPDDR4X\",\"vdd1Steps\":999,\"vdd2Steps\":0,\"vddqSteps\":0}", "failed", "invalidArgument");
    control(PC_COMMAND_FIRMWARE_UPGRADE_REQUEST, "{\"action\":\"mcu_begin\"}", "failed", "invalidState");
    download_state = OTA_DOWNLOAD_RECEIVING;
    control(PC_COMMAND_POWER_OFF, "{}", "failed", "busy");
    download_state = OTA_DOWNLOAD_IDLE;

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
    const cJSON *measurement = cJSON_GetObjectItemCaseSensitive(chip, "measurementData");
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
    start_expect(POWER_START, NULL); /* 相同训练配置且reboot=false不增加虚拟冷重启。 */
    CHECK(v2_power_sim_cycles() == cycles);
    result_is(0U, NULL, "");
    start_expect(CHANGED_START, NULL); /* 变更目标则MCU强制虚拟冷重启。 */
    CHECK(v2_power_sim_cycles() == cycles + 1U);
    body = query(PC_COMMAND_QUERY_POWER_MONITOR, PC_COMMAND_POWER_MONITOR_RESP);
    stats = cJSON_GetObjectItemCaseSensitive(body, "statistics");
    CHECK(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(stats, "voltageAvgMv"), 0)->valuedouble == 1810.0);
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
    CHECK(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(stats, "currentAvgMa"), 0)->valuedouble == 299.0);
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
    cJSON *chip = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(body, "testResults"), 0);
    cJSON *data = cJSON_GetObjectItemCaseSensitive(chip, "measurementData");
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
    chip = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(body, "testResults"), 0);
    data = cJSON_GetObjectItemCaseSensitive(chip, "measurementData");
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(data, "Vdd1Ave")));
    number_is(data, "Idd1Ave", 123U); number_is(chip, "testStatus", 1U);
    CHECK(measured_stats.state == IDD_STATS_TIMEOUT);
    cJSON_Delete(body);

    /* 最大整数、最长转义标签和64位错误/耗时仍必须装入完整响应，不能断连。 */
    memset(site_label, 1, sizeof(site_label) - 1U); site_label[15] = 0;
    start_expect(START_JSON, NULL);
    b1_sample(3U, INT32_MIN, UINT32_MAX);
    result_is(UINT64_MAX, "0xFFFFFFFFFFFFFFFF", site_label);
    memcpy(site_label, "SITE-7", 7U);
    message("PASS: PC RK service B1 real-path guards, 100 queries, 18 metrics, partial samples and freeze/reset\n");
}
#endif

void test_main(void)
{
    pc_dispatch_response_t response;
    CHECK(!pc_test_service_busy());
    confirmed = 0;
    start_expect(START_JSON, "otaNotReady");
    CHECK(fru_calls == 0U && queue_calls == 0U);
    confirmed = 1;
#if !defined(ENABLE_V2_POWER_SIMULATION) || !ENABLE_V2_POWER_SIMULATION
    start_expect("{\"algorithmType\":\"functional\",\"algorithmName\":\"mats\",\"algorithmIndex\":37,\"power\":{\"memoryType\":\"LPDDR4X\",\"vdd1Steps\":0,\"vdd2Steps\":0,\"vddqSteps\":0},\"reboot\":true,\"bootMode\":\"emmc\"}", "notReady");
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
    start_expect("{\"algorithmType\":\"functional\",\"algorithmName\":\"unknown\",\"algorithmIndex\":37,\"power\":null,\"reboot\":false,\"bootMode\":null}", "notSupported");
    start_expect("{\"algorithmType\":\"parametric\",\"algorithmName\":\"idd_0\",\"algorithmIndex\":37,\"power\":null,\"reboot\":false,\"bootMode\":null}", "notSupported");
    const ota_download_state_t locked[] = {OTA_DOWNLOAD_RECEIVING, OTA_DOWNLOAD_READY, OTA_DOWNLOAD_COMMIT_UNKNOWN};
    for (unsigned i = 0U; i < sizeof(locked) / sizeof(locked[0]); ++i) {
        download_state = locked[i];
        start_expect(START_JSON, "busy");
        CHECK(fru_calls == 0U && queue_calls == 0U && !pc_test_service_busy());
    }
    download_state = OTA_DOWNLOAD_IDLE;
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
    CHECK(pc_test_service_start(&view, 17U, &response) == PC_DISPATCH_ERROR);
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

    /* 在途时电源/Boot/OTA写入全部拒绝；心跳与OTA只读查询仍可使用。 */
    const uint16_t commands[] = {PC_COMMAND_POWER_ON, PC_COMMAND_POWER_OFF, PC_COMMAND_RESET_RK,
        PC_COMMAND_SET_BOOT_MODE, PC_COMMAND_SET_DDR_CONFIG, PC_COMMAND_FIRMWARE_UPGRADE_REQUEST};
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
        string_is(body, "errorCode", "busy"); cJSON_Delete(body); cJSON_free(json); cJSON_Delete(root);
    }
    CHECK(ota_calls == 0U);
    CHECK(dispatch(PC_COMMAND_CLIENT_HEARTBEAT, "{}", 18U, &response) == PC_DISPATCH_RESPONSE_READY);
    root = decode(&response, PC_COMMAND_SERVER_HEARTBEAT, 18U); CHECK(root->child == NULL); cJSON_Delete(root);
    CHECK(dispatch(PC_COMMAND_FIRMWARE_UPGRADE_REQUEST, "{\"action\":\"mcu_query\"}", 18U, &response) == PC_DISPATCH_RESPONSE_READY);
    CHECK(ota_calls == 1U && pc_test_service_busy());

    /* 错误cookie、无连接轮询均不能偷走当前连接的合法结果。 */
    inject(RK_RESPONSE_COMPLETE, 0U); queued_response.cookie++;
    CHECK(pc_dispatch_next_response(18U, &response) == 0 && pc_test_service_busy());
    inject(RK_RESPONSE_COMPLETE, 0U);
    pc_dispatch_poll(); CHECK(response_ready && pc_test_service_busy());
    response_ready = 0;
    result_is(0U, NULL, "SITE-7");
#if defined(B1_RK_UART)
    b1_measurement_check();
#endif
    start_expect(START_JSON, NULL);
    CHECK(queued_request.cookie != first_cookie);
    result_is(UINT64_C(0x123456789ABCDEF0), "0x123456789ABCDEF0", "SITE-7");
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
    start_expect("{\"algorithmType\":\"functional\",\"algorithmName\":\"mats\",\"algorithmIndex\":37,\"power\":null,\"reboot\":false,\"bootMode\":null,\"timeoutMs\":1200000}", NULL);
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
    finish(0U);
}
