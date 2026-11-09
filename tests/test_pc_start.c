/* 在QEMU执行正式0103解析、分发和组帧；硬件入口一旦被调用立即判失败。 */
#include "pc_dispatch.h"
#include "pc_state_machine.h"
#include "pc_test_message.h"
#include "pc_tester_info.h"
#include "monitor_task.h"
#include "result_led.h"
#if defined(B1_RK_UART)
#include "pc_trace.h"
#endif
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S1(x) #x
#define S(x) S1(x)
#define CHECK(c) do { if (!(c)) fail("FAIL " S(__LINE__) ": " #c "\n"); } while (0)
#define VALID_JSON "{\"algorithmType\":\"parametric\",\"algorithmName\":\"idd_0\",\"algorithmIndex\":1,\"power\":{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1800\",\"vdd2\":\"1100\",\"vddq\":\"600\"},\"reboot\":true,\"bootMode\":\"emmc\",\"TimingParameters\":{}}"
static int query_monitor;

/* 离线测试使用newlib有界堆；正式固件仍使用FreeRTOS堆。 */
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

/* 对双颗粒JSON每个分配点逐一注入失败，检查第二颗及测量对象失败时无泄漏。 */
static unsigned allocation_calls, allocation_fail_at, allocations_live;
static void *fault_allocate(size_t bytes)
{
    if (++allocation_calls == allocation_fail_at) return NULL;
    void *memory = malloc(bytes);
    if (memory) ++allocations_live;
    return memory;
}
static void fault_free(void *memory)
{
    if (memory) { CHECK(allocations_live > 0U); --allocations_live; free(memory); }
}
static void test_result_capacity_and_allocation(void)
{
    char site[16]; memset(site, 1, 15U); site[15] = '\0';
    const pc_functional_die_result_t dies[2] = {{UINT64_MAX, "SAF", 1U}, {UINT64_MAX, "", 1U}};
    idd_stats_t stats = {0};
    for (unsigned rail = 0U; rail < 3U; ++rail) {
        stats.voltage[rail].valid_count = stats.current[rail].valid_count = 1U;
        stats.voltage[rail].mean = stats.voltage[rail].minimum = stats.voltage[rail].maximum = INT32_MIN;
        stats.current[rail].mean = stats.current[rail].minimum = stats.current[rail].maximum = UINT32_MAX;
        stats.current[rail].sum = UINT32_MAX;
    }
    unsigned reached_success = 0U;
    cJSON_Hooks hooks = {fault_allocate, fault_free};
    cJSON_InitHooks(&hooks);
    for (allocation_fail_at = 1U; allocation_fail_at < 256U; ++allocation_fail_at) {
        allocation_calls = 0U;
        cJSON *body = pc_functional_result_create("gtest-stuck-address", UINT32_MAX, site, UINT64_MAX, dies);
        if (body && pc_test_add_measurement(body, &stats)) {
            char json[PC_DISPATCH_RESPONSE_CAPACITY - PC_PROTOCOL_HEADER_SIZE];
            CHECK(cJSON_PrintPreallocated(body, json, sizeof(json), 0));
            CHECK(strlen(json) + PC_PROTOCOL_HEADER_SIZE <= PC_DISPATCH_RESPONSE_CAPACITY);
            reached_success = 1U;
        }
        cJSON_Delete(body);
        CHECK(allocations_live == 0U);
        if (reached_success) break;
    }
    cJSON_InitHooks(NULL);
    CHECK(reached_success);
    message("PASS: dual-die worst-case PDU and every JSON allocation failure without leaks\n");
}

/* 两个µA样本的平均值允许不足1µA；0602和0105始终使用mA数值，空轨仍为null。 */
static void test_measurement_units(void)
{
    idd_stats_t stats = {0};
    stats.state = IDD_STATS_RUNNING;
    stats.current[0].valid_count = 2U;
    stats.current[0].sum = 1000;
    stats.current[0].mean = 500;
    stats.current[0].minimum = 100;
    stats.current[0].maximum = 900;
    stats.current[1] = stats.current[0];
    stats.current[1].sum = 1001;
    stats.current[1].maximum = 901;
    stats.voltage[0].valid_count = 1U;
    stats.voltage[0].mean = stats.voltage[0].minimum = stats.voltage[0].maximum = 1800;
    cJSON *body = cJSON_CreateObject();
    CHECK(body && cJSON_AddNullToObject(body, "measurementData"));
    CHECK(pc_test_add_measurement(body, &stats) && pc_test_add_statistics(body, &stats));
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(body, "measurementData");
    CHECK(cJSON_GetObjectItemCaseSensitive(data, "Vdd1Ave")->valuedouble == 1800.0);
    CHECK(cJSON_GetObjectItemCaseSensitive(data, "Idd1Ave")->valuedouble == 0.5);
    CHECK(fabs(cJSON_GetObjectItemCaseSensitive(data, "Idd1Min")->valuedouble - 0.1) < 1e-12);
    CHECK(fabs(cJSON_GetObjectItemCaseSensitive(data, "Idd1Peak")->valuedouble - 0.9) < 1e-12);
    CHECK(fabs(cJSON_GetObjectItemCaseSensitive(data, "Idd2Ave")->valuedouble - 0.5005) < 1e-12);
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(data, "IddqAve")));
    const cJSON *statistics = cJSON_GetObjectItemCaseSensitive(body, "statistics");
    CHECK(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(statistics, "currentAvgMa"), 0)->valuedouble == 0.5);
    CHECK(fabs(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(statistics, "currentAvgMa"), 1)->valuedouble - 0.5005) < 1e-12);
    CHECK(cJSON_IsNull(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(statistics, "currentPeakMa"), 2)));
    /* 校验实际发送文本，避免对象中的小数正确、链接库却无法输出JSON数字。 */
    char wire[2048];
    CHECK(cJSON_PrintPreallocated(body, wire, sizeof(wire), 0));
    cJSON *decoded = cJSON_Parse(wire);
    if (decoded == NULL) { message(wire); message("\n"); }
    CHECK(decoded != NULL);
    const cJSON *decoded_data = cJSON_GetObjectItemCaseSensitive(decoded, "measurementData");
    CHECK(fabs(cJSON_GetObjectItemCaseSensitive(decoded_data, "Idd2Ave")->valuedouble - 0.5005) < 1e-12);
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(decoded_data, "IddqAve")));
    cJSON_Delete(decoded);
    cJSON_Delete(body);
    message("PASS: current statistics keep microamp precision and JSON reports fractional milliamps\n");
}

/* 解析回归不执行硬件；任何电源、DAC、采样、Flash或其他业务调用均失败。 */
#if defined(B1_RK_UART)
void pc_trace_power_phase(const struct pc_power_diagnostic *diagnostic)
{ (void)diagnostic; fail("FAIL unexpected power log\n"); }
#endif
uint32_t HAL_GetTick(void) { if (query_monitor) return 1000U; fail("FAIL unexpected HAL_GetTick\n"); }
pwr_ctrl_state_t pwr_ctrl_get_state(void) { fail("FAIL unexpected power access\n"); }
pwr_ctrl_result_t monitor_request_power_on(uint32_t now) { (void)now; fail("FAIL power on\n"); }
uint8_t pwr_ctrl_is_configured(void) { fail("FAIL unexpected power configuration access\n"); }
pwr_ctrl_result_t monitor_request_training_power_on(ddr_memory_type_t type, const uint16_t mv[3], uint32_t now)
{ (void)type; (void)mv; (void)now; fail("FAIL training power on\n"); }
pwr_ctrl_result_t monitor_request_power_off(uint32_t now) { (void)now; fail("FAIL power off\n"); }
monitor_boot_result_t monitor_set_boot_mode(boot_mode_t mode) { (void)mode; fail("FAIL Boot write\n"); }
monitor_boot_result_t monitor_get_boot_mode(boot_mode_t *mode, uint16_t *mv)
{ (void)mode; (void)mv; fail("FAIL Boot query\n"); }
pwr_ctrl_result_t monitor_apply_ddr_config(ddr_memory_type_t type, const int16_t steps[3])
{ (void)type; (void)steps; fail("FAIL DDR write\n"); }
int monitor_get_latest(monitor_snapshot_t *out)
{
    if (!query_monitor) fail("FAIL monitor access\n");
    memset(out, 0, sizeof(*out));
    out->timestamp_ms = 950U;
    out->power_state = PWR_CTRL_STATE_ON;
    out->memory.valid_mask = MEMORY_MONITOR_VALID_VOLTAGE | MEMORY_MONITOR_VALID_IDD;
    const int32_t mv[] = {1800, 1100, 600};
    for (size_t rail = 0U; rail < 3U; rail++) {
        out->memory.voltage_mv[rail] = mv[rail];
        /* 第二轨取最小非零码，证明低于1mA的量不会被旧current_ma字段截成0。 */
        uint16_t raw = rail == 1U ? 1U : 0x8000U;
        out->memory.idd[rail].current_ma = pac1934_current_ma(raw, PAC1934_RSENSE_B1_UOHM);
        out->memory.idd[rail].current_ua = pac1934_current_ua(raw, PAC1934_RSENSE_B1_UOHM);
        out->memory.idd[rail].power_mw = pac1934_power_mw(0x08000000U, PAC1934_RSENSE_B1_UOHM);
    }
    return 1;
}
int monitor_idd_get(idd_stats_t *out)
{
    if (!query_monitor) fail("FAIL statistics access\n");
    memset(out, 0, sizeof(*out));
    return 1;
}
HAL_StatusTypeDef result_led_set(result_led_target_t target, result_led_state_t state)
{ (void)target; (void)state; fail("FAIL LED write\n"); }
HAL_StatusTypeDef result_led_get_all(result_led_state_t states[3])
{ (void)states; fail("FAIL LED query\n"); }
int pc_tester_info_load(fru_info_t *fru) { (void)fru; fail("FAIL Flash access\n"); }

static cJSON *base_request(void)
{
    cJSON *root = NULL;
    CHECK(pc_json_parse_object((const uint8_t *)VALID_JSON, strlen(VALID_JSON), &root, NULL) == PC_JSON_OK);
    return root;
}

/* 使用真实分发入口和响应JSON核对错误语义、独立Seq及index回带。 */
static void check_request_command(uint16_t command, cJSON *root,
                                 const char *error, int has_index, uint32_t index)
{
    char *json = cJSON_PrintUnformatted(root);
    CHECK(json != NULL);
    pc_protocol_frame_t request = {
        .command_id = command, .service_id = 0U, .sequence = 99U,
        .json = (const uint8_t *)json, .json_length = (uint32_t)strlen(json)
    };
    pc_dispatch_response_t response;
    CHECK(pc_dispatch_handle(&request, 17U, &response) == PC_DISPATCH_RESPONSE_READY);
    CHECK(response.data[10] == 0U && response.data[11] == 6U);
    CHECK(response.data[12] == 0U && response.data[13] == 0U &&
          response.data[14] == 0U && response.data[15] == 17U);
    cJSON *body = NULL;
    CHECK(pc_json_parse_object(response.data + 18U, response.length - 18U, &body, NULL) == PC_JSON_OK);
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(body, "status")->valuestring, "failed") == 0);
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(body, "errorCode")->valuestring, error) == 0);
    CHECK(cJSON_GetObjectItemCaseSensitive(body, "requestCommandId")->valuedouble == command);
    const cJSON *echo = cJSON_GetObjectItemCaseSensitive(body, "algorithmIndex");
    if (has_index < 0) CHECK(cJSON_IsNull(echo));
    else CHECK(has_index ? cJSON_IsNumber(echo) && echo->valuedouble == (double)index : echo == NULL);
    CHECK(ddr_power_get_memory_type() == DDR_MEMORY_LPDDR4X);
    CHECK(ddr_power_get_mv(DDR_RAIL_VDD1) == 1800U && ddr_power_get_mv(DDR_RAIL_VDD2) == 1100U &&
          ddr_power_get_mv(DDR_RAIL_VDDQ) == 600U);
    cJSON_Delete(body);
    cJSON_free(json);
    cJSON_Delete(root);
}

static void check_request(cJSON *root, const char *error, int has_index, uint32_t index)
{
    check_request_command(PC_COMMAND_START_TEST, root, error, has_index, index);
}

static void replace(cJSON *root, const char *key, cJSON *value)
{
    CHECK(value && cJSON_ReplaceItemInObjectCaseSensitive(root, key, value));
}

static cJSON *external_request(void)
{
    cJSON *root = base_request();
    replace(root, "algorithmType", cJSON_CreateString("functional"));
    replace(root, "algorithmName", cJSON_CreateString("gtest-xor"));
    replace(root, "power", cJSON_CreateNull());
    replace(root, "bootMode", cJSON_CreateNull());
    replace(root, "reboot", cJSON_CreateBool(0));
    return root;
}

/* 三类开始都须带时序对象；空对象表示默认，子字段可部分填写，不猜测单位或RK编码。 */
static void test_timing_parameters(void)
{
    static const char *const valid[] = {
        "{}", "{\"Freq\":\"\"}", "{\"Freq\":\"1600\",\"tRCD\":\"18\"}",
        "{\"CASReadLatency\":\"20\"}", "{\"CASWriteLatency\":\"12\"}",
        "{\"CASReadLatency\":\"20\",\"CASWriteLatency\":\"12\"}",
        "{\"Freq\":\"\",\"CASReadLatency\":\"\",\"CASWriteLatency\":\"\",\"tREFI\":\"\",\"tWR\":\"\",\"tRTP\":\"\","
        "\"tRCD\":\"\",\"tRP\":\"\",\"tRASmin\":\"\",\"tRASmax\":\"\",\"tWTR\":\"\",\"tRFCmin\":\"\","
        "\"tFAW\":\"\",\"tCCD\":\"\",\"tRRD\":\"\",\"BL\":\"\"}",
        "{\"Freq\":\"9223372036854775807\",\"tWR\":\"4294967295\",\"BL\":\"0\"}",
        "{\"Freq\":\"-9223372036854775808\",\"tRASmin\":\"000123\"}"
    };
    pc_test_start_view_t view = {0};
    for (size_t i = 0U; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        cJSON *root = external_request();
        replace(root, "TimingParameters", cJSON_Parse(valid[i]));
        CHECK(cJSON_AddNumberToObject(root, "timeoutMs", 1000));
        CHECK(pc_test_start_parse(root, &view) == PC_TEST_JSON_OK);
        CHECK(view.external_ready && view.timeout_ms == 1000U);
        CHECK(cJSON_IsObject(view.timing_parameters));
        cJSON_Delete(root);

        root = base_request();
        replace(root, "TimingParameters", cJSON_Parse(valid[i]));
        CHECK(pc_test_start_parse(root, &view) == PC_TEST_JSON_OK);
        CHECK(cJSON_IsObject(view.timing_parameters));
        check_request_command(PC_COMMAND_PARA_START, root, "notReady", 1, 1U);

        root = base_request();
        replace(root, "algorithmType", cJSON_CreateString("slt"));
        replace(root, "algorithmName", cJSON_CreateString("memtest86"));
        replace(root, "algorithmIndex", cJSON_CreateNull());
        replace(root, "TimingParameters", cJSON_Parse(valid[i]));
        CHECK(pc_slt_start_parse(root, &view) == PC_TEST_JSON_OK);
        CHECK(cJSON_IsObject(view.timing_parameters));
        cJSON_Delete(root);
    }
    /* 数值不经过double：完整64位频率与32位时序端点保持原值，空串不置位。 */
    cJSON *exact = base_request();
    replace(exact, "TimingParameters", cJSON_Parse("{\"Freq\":\"-9223372036854775808\",\"tWR\":\"4294967295\",\"tRCD\":\"\",\"BL\":\"0\"}"));
    CHECK(pc_test_start_parse(exact, &view) == PC_TEST_JSON_OK);
    CHECK(view.timing.frequency == INT64_MIN && view.timing.values[3] == UINT32_MAX);
    CHECK(view.timing.present == ((1U << 3) | (1U << 14)) && view.timing.values[14] == 0U);
    CHECK(pc_timing_command(0U) == 0x25U && pc_timing_command(1U) == 0x26U);
    CHECK(pc_timing_command(2U) == 0x28U && pc_timing_command(14U) == 0x34U && pc_timing_command(15U) == 0U);
    cJSON_Delete(exact);
    const uint16_t commands[] = {PC_COMMAND_START_TEST, PC_COMMAND_PARA_START, PC_COMMAND_SLT_START};
    for (size_t kind = 0U; kind < sizeof(commands) / sizeof(commands[0]); ++kind) {
        for (unsigned null_value = 0U; null_value < 2U; ++null_value) {
            cJSON *root = kind == 0U ? external_request() : base_request();
            if (kind == 2U) {
                replace(root, "algorithmType", cJSON_CreateString("slt"));
                replace(root, "algorithmName", cJSON_CreateString("memtest86"));
                replace(root, "algorithmIndex", cJSON_CreateNull());
            }
            if (null_value) replace(root, "TimingParameters", cJSON_CreateNull());
            else cJSON_DeleteItemFromObjectCaseSensitive(root, "TimingParameters");
            pc_test_start_view_t before = view;
            CHECK((kind == 2U ? pc_slt_start_parse(root, &view) : pc_test_start_parse(root, &view)) != PC_TEST_JSON_OK);
            CHECK(memcmp(&view, &before, sizeof(view)) == 0);
            check_request_command(commands[kind], root, "invalidArgument", kind == 2U ? -1 : 1, 1U);
        }
    }
    static const char *const invalid[] = {
        "0", "[]", "\"1600\"", "{\"freq\":\"1600\"}", "{\"unknown\":\"1\"}",
        "{\"Freq\":1600}", "{\"Freq\":null}", "{\"Freq\":true}",
        "{\"Freq\":{}}", "{\"Freq\":[]}", "{\"Freq\":\"1\",\"Freq\":\"2\"}",
        "{\"CASLatency\":\"20\"}", "{\"casReadLatency\":\"20\"}",
        "{\"CASReadLatency\":20}", "{\"CASWriteLatency\":null}",
        "{\"CASReadLatency\":\"20\",\"CASReadLatency\":\"24\"}",
        "{\"CASWriteLatency\":\"12\",\"CASWriteLatency\":\"16\"}",
        "{\"CASReadLatency\":\"20\",\"CASWriteLatency\":\"12\",\"CASLatency\":\"20\"}",
        "{\"tRC\":\"\"}", "{\"ODT\":\"\"}", "{\"tRAS\":\"1\"}", "{\"tRFC\":\"1\"}",
        "{\"BurstLength\":\"16\"}", "{\"tRASMin\":\"1\"}", "{\"tWR\":\"-1\"}",
        "{\"tWR\":\"4294967296\"}", "{\"tWR\":\"1.0\"}", "{\"Freq\":\"+1\"}",
        "{\"Freq\":\" 1600\"}", "{\"Freq\":\"1e3\"}", "{\"Freq\":\"0x640\"}",
        "{\"Freq\":\"9223372036854775808\"}", "{\"Freq\":\"-9223372036854775809\"}"
    };
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        for (size_t kind = 0U; kind < sizeof(commands) / sizeof(commands[0]); ++kind) {
            cJSON *root = base_request();
            if (kind == 1U) {
                replace(root, "algorithmType", cJSON_CreateString("parametric"));
                replace(root, "algorithmName", cJSON_CreateString("idd_0"));
            } else if (kind == 2U) {
                replace(root, "algorithmType", cJSON_CreateString("slt"));
                replace(root, "algorithmName", cJSON_CreateString("memtest86"));
                replace(root, "algorithmIndex", cJSON_CreateNull());
            }
            replace(root, "TimingParameters", cJSON_Parse(invalid[i]));
            pc_test_start_view_t before = view;
            CHECK((kind == 2U ? pc_slt_start_parse(root, &view) : pc_test_start_parse(root, &view)) != PC_TEST_JSON_OK);
            CHECK(memcmp(&view, &before, sizeof(view)) == 0);
            check_request_command(commands[kind], root, "invalidArgument", kind == 2U ? -1 : 1, 1U);
        }
    }
    cJSON *root = base_request();
    CHECK(cJSON_AddObjectToObject(root, "TimingParameters"));
    CHECK(pc_test_start_parse(root, &view) != PC_TEST_JSON_OK);
    check_request(root, "invalidArgument", 1, 1U);
    message("PASS: required TimingParameters object in FUNC/PARA/SLT, empty/partial values, null/missing rejection\n");
}

/* 只验证协议层：保持现状不会被转换为0V请求，未知测量值不能伪装成实测0。 */
static void test_message_contract(void)
{
    pc_test_start_view_t view;
    cJSON *root = external_request();
    CHECK(pc_test_start_parse(root, &view) == PC_TEST_JSON_OK);
    CHECK(view.external_ready == 1U && view.reboot == 0U && view.timeout_ms == 0U);
    CHECK(!view.memory_type && !view.boot_mode && view.target_mv[0] == 0U &&
          view.target_mv[1] == 0U && view.target_mv[2] == 0U);
    cJSON_Delete(root);
    const uint32_t timeouts[] = {0U, 1U, 1200000U, 86400000U};
    for (size_t mode = 0U; mode < 2U; mode++) {
        for (size_t i = 0U; i < sizeof(timeouts) / sizeof(timeouts[0]); i++) {
            root = mode ? external_request() : base_request();
            CHECK(cJSON_AddNumberToObject(root, "timeoutMs", timeouts[i]));
            CHECK(pc_test_start_parse(root, &view) == PC_TEST_JSON_OK);
            CHECK(view.external_ready == mode && view.timeout_ms == timeouts[i]);
            cJSON_Delete(root);
        }
    }
    const double bad_timeouts[] = {-1.0, 0.5, 86400001.0, 4294967296.0, NAN, INFINITY};
    for (size_t i = 0U; i < sizeof(bad_timeouts) / sizeof(bad_timeouts[0]); i++) {
        root = external_request();
        CHECK(cJSON_AddNumberToObject(root, "timeoutMs", 1));
        cJSON_GetObjectItemCaseSensitive(root, "timeoutMs")->valuedouble = bad_timeouts[i];
        pc_test_start_view_t before = view;
        CHECK(pc_test_start_parse(root, &view) != PC_TEST_JSON_OK &&
              memcmp(&view, &before, sizeof(view)) == 0);
        cJSON_Delete(root);
    }
    for (size_t i = 0U; i < 6U; i++) {
        root = external_request();
        if (i == 0U) replace(root, "reboot", cJSON_CreateBool(1));
        if (i == 1U) replace(root, "bootMode", cJSON_CreateString("emmc"));
        if (i == 2U) replace(root, "power", cJSON_CreateObject());
        if (i == 3U) CHECK(cJSON_AddBoolToObject(root, "timeoutMs", 0));
        if (i == 4U) CHECK(cJSON_AddNullToObject(root, "timeoutMs"));
        if (i == 5U) {
            CHECK(cJSON_AddNumberToObject(root, "timeoutMs", 1));
            CHECK(cJSON_AddNumberToObject(root, "timeoutMs", 2));
        }
        CHECK(pc_test_start_parse(root, &view) != PC_TEST_JSON_OK);
        cJSON_Delete(root);
    }
    /* 原始文本入口拒绝键和值内的NUL，防止cJSON字符串截断后绕过命令表。 */
    const char *const nul_json[] = {
        "{\"algorithmName\":\"gtest-xor\\u0000junk\"}",
        "{\"algorithmName\\u0000junk\":\"gtest-xor\"}"
    };
    for (size_t i = 0U; i < 2U; i++) {
        root = NULL;
        CHECK(pc_json_parse_object((const uint8_t *)nul_json[i], strlen(nul_json[i]),
                                   &root, NULL) == PC_JSON_ERROR_SYNTAX && root == NULL);
    }
    pc_functional_die_result_t dies[PC_FUNCTIONAL_DIE_COUNT] = {{0U, "", 0U}, {0U, "", 0U}};
    const uint64_t addresses[] = {0U, UINT64_C(0x0000000123456780), UINT64_MAX};
    const char *const address_text[] = {"0x0000000000000000", "0x0000000123456780", "0xFFFFFFFFFFFFFFFF"};
    for (unsigned mask = 0U; mask < 4U; ++mask) {
        for (size_t i = 0U; i < 3U; ++i) {
            for (unsigned die = 0U; die < PC_FUNCTIONAL_DIE_COUNT; ++die) {
                dies[die].failed = (mask >> die) & 1U;
                dies[die].error_address = addresses[i];
                dies[die].error_type = "SAF";
            }
            root = pc_functional_result_create("gtest-xor", UINT32_MAX, "B1-01", 42000U, dies);
            CHECK(root && cJSON_GetArraySize(root) == 7);
            CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(root, "siteId")->valuestring, "B1-01") == 0);
            CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(root, "algorithmType")->valuestring, "functional") == 0);
            CHECK(cJSON_GetObjectItemCaseSensitive(root, "algorithmIndex")->valuedouble == 4294967295.0);
            CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(root, "measurementData")));
            const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "testResults");
            CHECK(cJSON_IsArray(array) && cJSON_GetArraySize(array) == 2);
            for (unsigned die = 0U; die < PC_FUNCTIONAL_DIE_COUNT; ++die) {
                const cJSON *entry = cJSON_GetArrayItem(array, die);
                CHECK(cJSON_GetArraySize(entry) == 4);
                CHECK(!cJSON_GetObjectItemCaseSensitive(entry, "siteId"));
                CHECK(!cJSON_GetObjectItemCaseSensitive(entry, "measurementData"));
                CHECK(cJSON_GetObjectItemCaseSensitive(entry, "mirrorIndex")->valueint == (int)die);
                CHECK(cJSON_GetObjectItemCaseSensitive(entry, "testStatus")->valueint == (dies[die].failed ? 0 : 1));
                CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(entry, "physicalAddr")->valuestring, "") == 0);
                const cJSON *desc = cJSON_GetObjectItemCaseSensitive(entry, "resultDesc");
                CHECK(cJSON_IsObject(desc) && cJSON_GetArraySize(desc) == 2);
                CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(desc, "errorType")->valuestring,
                             dies[die].failed ? "SAF" : "") == 0);
                const cJSON *address = cJSON_GetObjectItemCaseSensitive(desc, "firstErrorAddress");
                CHECK(dies[die].failed ? cJSON_IsString(address) && strcmp(address->valuestring, address_text[i]) == 0
                                      : cJSON_IsNull(address));
            }
            cJSON_Delete(root);
        }
    }
    /* 核对线上数字文本，不能用double比较掩盖uint64大值的末位丢失。 */
    const uint64_t times[] = {0U, 42000U, UINT64_C(4294967296), UINT64_C(9007199254740993), UINT64_MAX};
    const char *const time_text[] = {"0", "42000", "4294967296", "9007199254740993", "18446744073709551615"};
    for (size_t i = 0U; i < sizeof(times) / sizeof(times[0]); ++i) {
        root = pc_functional_result_create("gtest-xor", 1U, "", times[i], dies);
        CHECK(root);
        char *json = cJSON_PrintUnformatted(root);
        CHECK(json);
        const char *value = strstr(json, "\"testTimeMs\":");
        CHECK(value);
        value += strlen("\"testTimeMs\":");
        CHECK(strncmp(value, time_text[i], strlen(time_text[i])) == 0 && value[strlen(time_text[i])] == ',');
        cJSON *parsed = cJSON_Parse(json);
        CHECK(parsed && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(parsed, "testTimeMs")));
        cJSON_Delete(parsed);
        cJSON_free(json);
        cJSON_Delete(root);
    }
    CHECK(pc_functional_result_create(NULL, 1U, "", 0U, dies) == NULL);
    CHECK(pc_functional_result_create("gtest-xor", 1U, NULL, 0U, dies) == NULL);
    CHECK(pc_functional_result_create("gtest-xor", 1U, "", 0U, NULL) == NULL);
    for (unsigned die = 0U; die < PC_FUNCTIONAL_DIE_COUNT; ++die) {
        dies[die].failed = 2U;
        CHECK(pc_functional_result_create("gtest-xor", 1U, "", 0U, dies) == NULL);
        dies[die].failed = 1U; dies[die].error_type = NULL;
        CHECK(pc_functional_result_create("gtest-xor", 1U, "", 0U, dies) == NULL);
        dies[die].error_type = "";
    }
    message("PASS: external-ready JSON, optional timeout, NUL rejection and real FUNC result\n");
}

/* 复用正式接收顺序：逐字节组帧、分发、消费响应后复位；禁止任何硬件副作用。 */
static void consume_pc_bytes(pc_sm_t *sm, const uint8_t *data, size_t length,
                             uint32_t counts[2])
{
    for (size_t i = 0U; i < length; ++i) {
        pc_sm_feed_result_t feed = pc_sm_feed(sm, data[i]);
        CHECK(feed != PC_SM_FEED_ERROR);
        if (feed != PC_SM_FEED_FRAME_READY) continue;
        pc_protocol_frame_t frame;
        pc_dispatch_response_t response;
        CHECK(pc_sm_get_frame(sm, &frame) == 0);
        const uint32_t sequence = counts[0] + counts[1] + 1U;
        CHECK(pc_dispatch_handle(&frame, sequence, &response) == PC_DISPATCH_RESPONSE_READY);
        CHECK(response.data[15] == sequence && response.data[12] == 0U &&
              response.data[13] == 0U && response.data[14] == 0U);
        cJSON *body = NULL;
        CHECK(pc_json_parse_object(response.data + 18U, response.length - 18U,
                                   &body, NULL) == PC_JSON_OK);
        CHECK(response.data[10] == 0U);
        if (response.data[11] == PC_COMMAND_SERVER_HEARTBEAT) {
            CHECK(cJSON_GetArraySize(body) == 0);
            ++counts[0];
        } else {
            CHECK(response.data[11] == PC_COMMAND_COMMAND_RESP);
            CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(body, "status")->valuestring, "failed") == 0);
            CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(body, "errorCode")->valuestring, "invalidArgument") == 0);
            CHECK(cJSON_GetObjectItemCaseSensitive(body, "requestCommandId")->valueint == frame.command_id);
            ++counts[1];
        }
        cJSON_Delete(body);
        pc_sm_reset(sm);
    }
}

static void test_stream_recovery(void)
{
    static uint8_t packet[PC_PROTOCOL_MAX_PDU];
    const uint8_t heartbeat[] = {
        0, 0, 0, 20, 0, 1, 0, 8, 0, 0, 0, 2, 0, 0, 0, 99, 0, 0, '{', '}'
    };
    uint8_t batch[48];
    pc_sm_t sm;
    uint32_t counts[2] = {0U, 0U};
    pc_sm_init(&sm);
    consume_pc_bytes(&sm, (const uint8_t *)"TEXT", 4U, counts);
    CHECK(!pc_sm_is_receiving(&sm) && sm.received == 0U);
    consume_pc_bytes(&sm, heartbeat, sizeof(heartbeat), counts);
    CHECK(counts[0] == 1U && counts[1] == 0U);

    /* 垃圾与两个好帧同批到达、跨任意两批拆分，不能漏帧或重复应答。 */
    memcpy(batch, "TEXT", 4U);
    memcpy(batch + 4U, heartbeat, sizeof(heartbeat));
    memcpy(batch + 24U, heartbeat, sizeof(heartbeat));
    memcpy(batch + 44U, "TEXT", 4U);
    for (size_t split = 0U; split <= sizeof(batch); ++split) {
        pc_sm_init(&sm); counts[0] = counts[1] = 0U;
        consume_pc_bytes(&sm, batch, split, counts);
        consume_pc_bytes(&sm, batch + split, sizeof(batch) - split, counts);
        CHECK(counts[0] == 2U && counts[1] == 0U && !pc_sm_is_receiving(&sm));
    }

    /* 各类坏头之后紧接好帧；候选前缀重叠时同样不得丢失真正的长度字段。 */
    const uint8_t offsets[] = {3U, 2U, 5U, 7U, 9U};
    const uint8_t values[] = {17U, 0x10U, 2U, 7U, 1U};
    for (size_t i = 0U; i < sizeof(offsets); ++i) {
        memcpy(batch, heartbeat, sizeof(heartbeat));
        batch[offsets[i]] = values[i];
        memcpy(batch + 20U, heartbeat, sizeof(heartbeat));
        pc_sm_init(&sm); counts[0] = counts[1] = 0U;
        consume_pc_bytes(&sm, batch, 40U, counts);
        CHECK(counts[0] == 1U && counts[1] == 0U);
    }
    pc_sm_init(&sm); counts[0] = counts[1] = 0U;
    consume_pc_bytes(&sm, (const uint8_t *)"\0\0", 2U, counts);
    consume_pc_bytes(&sm, heartbeat, sizeof(heartbeat), counts);
    CHECK(counts[0] == 1U && counts[1] == 0U);

    /* 头和长度有效时，坏JSON只回错；控制及历史升级编号均不得产生硬件副作用。 */
    const char *const invalid_json[] = {"TEXT", "{\"x\":}", "[]", "{}x",
        "{\"x\":1,\"x\":2}", "{\"x\":\"\xED\xA0\x80\"}", "{\"x\":\"\\u0000\"}"};
    const uint16_t commands[] = {PC_COMMAND_CLIENT_HEARTBEAT, PC_COMMAND_START_TEST, PC_COMMAND_PARA_START,
                                PC_COMMAND_POWER_ON, PC_COMMAND_FIRMWARE_UPGRADE_REQUEST};
    for (size_t c = 0U; c < sizeof(commands) / sizeof(commands[0]); ++c) {
        for (size_t j = 0U; j < sizeof(invalid_json) / sizeof(invalid_json[0]); ++j) {
            uint32_t length;
            CHECK(pc_protocol_build_response(commands[c], 99U, (const uint8_t *)invalid_json[j],
                (uint32_t)strlen(invalid_json[j]), packet, sizeof(packet), &length) == 0);
            pc_sm_init(&sm); counts[0] = counts[1] = 0U;
            consume_pc_bytes(&sm, packet, length, counts);
            consume_pc_bytes(&sm, heartbeat, sizeof(heartbeat), counts);
            CHECK(counts[0] == 1U && counts[1] == 1U);
        }
    }
    memcpy(packet, heartbeat, sizeof(heartbeat)); packet[3] = 18U;
    pc_sm_init(&sm); counts[0] = counts[1] = 0U;
    consume_pc_bytes(&sm, packet, 18U, counts);
    consume_pc_bytes(&sm, heartbeat, sizeof(heartbeat), counts);
    CHECK(counts[0] == 1U && counts[1] == 1U);

    /* 不扫描已认可的正文：即使其中藏着完整心跳，也只能拒绝外层坏JSON。 */
    uint32_t length;
    CHECK(pc_protocol_build_response(PC_COMMAND_POWER_ON, 99U, heartbeat, sizeof(heartbeat),
                                     packet, sizeof(packet), &length) == 0);
    pc_sm_init(&sm); counts[0] = counts[1] = 0U;
    consume_pc_bytes(&sm, packet, length, counts);
    CHECK(counts[0] == 0U && counts[1] == 1U);
    consume_pc_bytes(&sm, heartbeat, sizeof(heartbeat), counts);
    CHECK(counts[0] == 1U && counts[1] == 1U);

    /* 4096字节合法帧仍完整接收；合法长度的半帧留给TCP超时，不抢执行内部字节。 */
    memcpy(packet, heartbeat, sizeof(heartbeat));
    packet[2] = 0x10U; packet[3] = 0U;
    memset(packet + 20U, ' ', sizeof(packet) - 20U);
    pc_sm_init(&sm); counts[0] = counts[1] = 0U;
    consume_pc_bytes(&sm, packet, sizeof(packet), counts);
    CHECK(counts[0] == 1U && counts[1] == 0U);
    pc_sm_init(&sm); counts[0] = counts[1] = 0U;
    consume_pc_bytes(&sm, packet, 20U, counts);
    consume_pc_bytes(&sm, heartbeat, sizeof(heartbeat), counts);
    CHECK(pc_sm_is_receiving(&sm) && counts[0] == 0U && counts[1] == 0U);

    /* 单次恢复最多丢弃4096字节；用常量实值锁定边界，不把无上限扫描算通过。 */
    pc_sm_init(&sm);
    for (size_t i = 0U; i < 4096U; ++i) CHECK(pc_sm_feed(&sm, 'T') == PC_SM_FEED_MORE);
    CHECK(pc_sm_feed(&sm, 'T') == PC_SM_FEED_ERROR && sm.state == PC_SM_STATE_ERROR);
    pc_sm_init(&sm); counts[0] = counts[1] = 0U;
    consume_pc_bytes(&sm, heartbeat, sizeof(heartbeat), counts);
    CHECK(counts[0] == 1U && counts[1] == 0U);
    message("PASS: actual PC stream recovery, malformed JSON, no embedded dispatch and bounded scan\n");
}

/* 穷举两种内存、每轨256个原生码，直接执行正式JSON解析和共享电压换算。 */
static void test_native_power_mv(void)
{
    const char *const types[] = {"LPDDR4", "LPDDR4X"};
    const char *const keys[] = {"vdd1", "vdd2", "vddq"};
    const int16_t minimum_steps[2][3] = {{-100, -140, -140}, {-100, -140, -40}};
    const uint16_t nominal[2][3] = {{1800U, 1100U, 1100U}, {1800U, 1100U, 600U}};
    const uint16_t minimum[] = {800U, 400U, 400U};
    const uint16_t step_mv[] = {10U, 5U, 5U};
    for (unsigned type = 0U; type < 2U; ++type) {
        for (unsigned rail = 0U; rail < 3U; ++rail) {
            cJSON *root = base_request();
            cJSON *power = cJSON_GetObjectItemCaseSensitive(root, "power");
            replace(power, "memoryType", cJSON_CreateString(types[type]));
            replace(power, "vddq", cJSON_CreateString(type == 0U ? "1100" : "600"));
            cJSON *number = cJSON_GetObjectItemCaseSensitive(power, keys[rail]);
            char text[16];
            pc_test_start_view_t view;
            for (unsigned code = 0U; code < 256U; ++code) {
                int16_t step = (int16_t)(minimum_steps[type][rail] + (int)code);
                snprintf(text, sizeof(text), "%u", minimum[rail] + code * step_mv[rail]);
                CHECK(cJSON_SetValuestring(number, text));
                CHECK(pc_test_start_parse(root, &view) == PC_TEST_JSON_OK);
                CHECK(view.power_steps[rail] == step);
                for (unsigned other = 0U; other < 3U; ++other) {
                    CHECK(view.target_mv[other] == (other == rail ?
                        minimum[rail] + code * step_mv[rail] : nominal[type][other]));
                }
            }
            /* 上下边界各越1档必须拒绝，失败不能交付部分目标给业务层。 */
            const uint16_t invalid[] = {(uint16_t)(minimum[rail] - step_mv[rail]),
                                        (uint16_t)(minimum[rail] + 256U * step_mv[rail]),
                                        (uint16_t)(nominal[type][rail] - 1U),
                                        (uint16_t)(nominal[type][rail] + 1U)};
            for (unsigned i = 0U; i < 4U; ++i) {
                snprintf(text, sizeof(text), "%u", invalid[i]);
                CHECK(cJSON_SetValuestring(number, text));
                memset(&view, 0xA5, sizeof(view));
                pc_test_start_view_t before = view;
                CHECK(pc_test_start_parse(root, &view) != PC_TEST_JSON_OK);
                CHECK(memcmp(&view, &before, sizeof(view)) == 0);
                const char *memory_type = "unchanged";
                int16_t steps[] = {1, 2, 3};
                uint16_t mv[] = {11, 22, 33};
                CHECK(pc_power_parse(power, &memory_type, steps, mv) != PC_TEST_JSON_OK);
                CHECK(strcmp(memory_type, "unchanged") == 0 && steps[0] == 1 && steps[1] == 2 && steps[2] == 3);
                CHECK(mv[0] == 11 && mv[1] == 22 && mv[2] == 33);
            }
            cJSON_Delete(root);
        }
    }
    message("PASS: string mV, 1536 exact targets, range/grid rejection and unchanged outputs\n");
}

/* PARA沿用FUNC的JSON校验；错误必须关联0108及合法index，不提前访问硬件。 */
static void test_para_message_contract(void)
{
    pc_test_start_view_t view;
    cJSON *root = base_request();
    CHECK(pc_test_start_parse(root, &view) == PC_TEST_JSON_OK);
    CHECK(strcmp(view.algorithm_type, "parametric") == 0 &&
          strcmp(view.algorithm_name, "idd_0") == 0);
    CHECK(view.target_mv[0] == 1800U && view.target_mv[1] == 1100U && view.target_mv[2] == 600U);
    check_request_command(PC_COMMAND_PARA_START, root, "notReady", 1, 1U);

    root = base_request();
    CHECK(cJSON_AddNumberToObject(root, "algorithmIndex", 2));
    check_request_command(PC_COMMAND_PARA_START, root, "invalidArgument", 0, 0U);
    root = base_request();
    replace(root, "algorithmIndex", cJSON_CreateNull());
    check_request_command(PC_COMMAND_PARA_START, root, "invalidArgument", 0, 0U);
    root = base_request();
    replace(root, "reboot", cJSON_CreateString("true"));
    check_request_command(PC_COMMAND_PARA_START, root, "invalidArgument", 1, 1U);
    root = base_request();
    replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "vddq", cJSON_CreateString("601"));
    check_request_command(PC_COMMAND_PARA_START, root, "invalidArgument", 1, 1U);
    root = base_request();
    replace(root, "power", cJSON_CreateNull());
    replace(root, "bootMode", cJSON_CreateNull());
    replace(root, "reboot", cJSON_CreateBool(0));
    CHECK(pc_test_start_parse(root, &view) == PC_TEST_JSON_OK && view.external_ready == 1U);
    check_request_command(PC_COMMAND_PARA_START, root, "notReady", 1, 1U);

    const pc_functional_die_result_t dies[2] = {{0U, "", 0U}, {UINT64_C(0x1234), "", 1U}};
    root = pc_algorithm_result_create("parametric", "idd_0", 8U, "3", 100U, dies);
    CHECK(root && strcmp(cJSON_GetObjectItemCaseSensitive(root, "algorithmType")->valuestring,
                         "parametric") == 0);
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(root, "algorithmName")->valuestring, "idd_0") == 0);
    CHECK(cJSON_GetObjectItemCaseSensitive(root, "algorithmIndex")->valueint == 8);
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(root, "measurementData")));
    const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "testResults");
    CHECK(cJSON_GetArraySize(results) == 2);
    CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(results, 0), "testStatus")->valueint == 1);
    CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(results, 1), "testStatus")->valueint == 1);
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(results, 0), "resultDesc")));
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(results, 1), "resultDesc")));
    cJSON_Delete(root);
    CHECK(pc_algorithm_result_create(NULL, "idd_0", 8U, "3", 100U, dies) == NULL);
    CHECK(pc_algorithm_result_create("slt", "idd_0", 8U, "3", 100U, dies) == NULL);
    message("PASS: PARA start JSON shares voltage/timing validation, index errors and result category\n");
}

void test_main(void)
{
    test_para_message_contract();
    test_timing_parameters();
    test_native_power_mv();
    test_result_capacity_and_allocation();
    test_measurement_units();
    test_stream_recovery();
    test_message_contract();
    const char *const required[] = {"algorithmType", "algorithmName", "algorithmIndex", "power", "reboot", "bootMode", "TimingParameters"};
    const char *const power_keys[] = {"memoryType", "vdd1", "vdd2", "vddq"};
    cJSON *root = base_request();
    pc_test_start_view_t view;
    CHECK(pc_test_start_parse(root, &view) == PC_TEST_JSON_OK);
    CHECK(view.target_mv[0] == 1800U && view.target_mv[1] == 1100U && view.target_mv[2] == 600U);
    CHECK(view.reboot == 1U && view.algorithm_index == 1U &&
          view.external_ready == 0U && view.timeout_ms == 0U);
    check_request(root, "notReady", 1, 1U);

    /* 三大类、两内存类型及七种Boot均为现有合法枚举，未知算法名不伪造RK编号。 */
    const char *const classes[] = {"functional", "slt", "parametric"};
    const char *const modes[] = {"usbMaskrom", "sdCard", "emmc", "fspiM0", "fspiM1", "fspiM2", "emmcSd"};
    for (size_t i = 0U; i < 7U; i++) {
        root = base_request();
        replace(root, "algorithmType", cJSON_CreateString(classes[i % 3U]));
        replace(root, "bootMode", cJSON_CreateString(modes[i]));
        replace(root, "algorithmName", cJSON_CreateString("未约定的算法"));
        replace(root, "reboot", cJSON_CreateBool(0));
        replace(root, "algorithmIndex", cJSON_CreateNumber(i == 0U ? 0.0 : 4294967295.0));
        replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "memoryType", cJSON_CreateString("LPDDR4"));
        replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "vddq", cJSON_CreateString("1100"));
        CHECK(pc_test_start_parse(root, &view) == PC_TEST_JSON_OK && view.target_mv[2] == 1100U && view.reboot == 0U);
        check_request(root, "notReady", 1, i == 0U ? 0U : UINT32_MAX);
    }
    for (size_t i = 0U; i < sizeof(required) / sizeof(required[0]); i++) {
        root = base_request();
        cJSON_DeleteItemFromObjectCaseSensitive(root, required[i]);
        check_request(root, "invalidArgument", i != 2U, 1U);
        root = base_request();
        CHECK(cJSON_AddItemToObject(root, required[i], cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(root, required[i]), 1)));
        check_request(root, "invalidArgument", i != 2U, 1U);
        root = base_request();
        replace(root, required[i], cJSON_CreateNull());
        check_request(root, "invalidArgument", i != 2U, 1U);
    }
    for (size_t i = 0U; i < 4U; i++) {
        root = base_request();
        cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "power"), power_keys[i]);
        check_request(root, "invalidArgument", 1, 1U);
        root = base_request();
        cJSON *power = cJSON_GetObjectItemCaseSensitive(root, "power");
        CHECK(cJSON_AddItemToObject(power, power_keys[i], cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(power, power_keys[i]), 1)));
        check_request(root, "invalidArgument", 1, 1U);
    }
    const double invalid_index[] = {-1.0, 1.5, 4294967296.0};
    for (size_t i = 0U; i < 3U; i++) {
        root = base_request(); replace(root, "algorithmIndex", cJSON_CreateNumber(invalid_index[i]));
        check_request(root, "invalidArgument", 0, 0U);
    }
    root = base_request(); replace(root, "algorithmIndex", cJSON_CreateBool(1));
    check_request(root, "invalidArgument", 0, 0U);
    root = base_request(); replace(root, "reboot", cJSON_CreateNumber(1));
    check_request(root, "invalidArgument", 1, 1U);
    root = base_request(); CHECK(cJSON_AddNumberToObject(root, "extra", 1));
    check_request(root, "invalidArgument", 1, 1U);
    root = base_request(); CHECK(cJSON_AddNumberToObject(cJSON_GetObjectItemCaseSensitive(root, "power"), "extra", 1));
    check_request(root, "invalidArgument", 1, 1U);
    for (size_t i = 0U; i < 2U; i++) {
        root = base_request(); replace(root, i == 0U ? "algorithmType" : "bootMode", cJSON_CreateString("UNKNOWN"));
        check_request(root, "invalidArgument", 1, 1U);
    }
    root = base_request(); replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "memoryType", cJSON_CreateString("LPDDR5"));
    check_request(root, "invalidArgument", 1, 1U);
    const char *const invalid_mv[] = {"", " ", " 1800", "1800 ", "+1800", "-1800", "1800.0",
        "1.8e3", "0x708", "1800mV", "1800x", "0", "65536", "999999999999999999999999",
        "1801", "1799", "790", "3360", "１８００"};
    for (size_t i = 0U; i < sizeof(invalid_mv) / sizeof(invalid_mv[0]); i++) {
        root = base_request(); replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "vdd1", cJSON_CreateString(invalid_mv[i]));
        check_request(root, "invalidArgument", 1, 1U);
    }
    root = base_request(); replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "vdd1", cJSON_CreateNumber(1800));
    check_request(root, "invalidArgument", 1, 1U);
    root = base_request(); replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "vddq", cJSON_CreateBool(1));
    check_request(root, "invalidArgument", 1, 1U);
    root = base_request(); replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "vddq", cJSON_CreateString("1680"));
    check_request(root, "invalidArgument", 1, 1U);
    root = base_request(); replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "vdd2", cJSON_CreateString("1680"));
    check_request(root, "invalidArgument", 1, 1U);
    root = base_request(); replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "vdd1", cJSON_CreateString("3350"));
    CHECK(pc_test_start_parse(root, &view) == PC_TEST_JSON_OK && view.target_mv[0] == 3350U);
    check_request(root, "notReady", 1, 1U); /* 可写但极端值也不执行，不能当成安全批准。 */
    root = base_request(); replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "vdd1", cJSON_CreateString("01800"));
    CHECK(pc_test_start_parse(root, &view) == PC_TEST_JSON_OK && view.target_mv[0] == 1800U);
    cJSON_Delete(root);
    /* 旧键和新旧混用均拒绝，不能把电压误当档位；错误仍关联原algorithmIndex。 */
    const char *const old_keys[] = {"vdd1Steps", "vdd2Steps", "vddqSteps"};
    for (unsigned i = 0U; i < 3U; ++i) {
        root = base_request(); cJSON *power = cJSON_GetObjectItemCaseSensitive(root, "power");
        cJSON_DeleteItemFromObjectCaseSensitive(power, power_keys[i + 1U]);
        CHECK(cJSON_AddNumberToObject(power, old_keys[i], 0));
        check_request(root, "invalidArgument", 1, 1U);
    }

    /* 直接构造非有限cJSON值，证明浮点转整数前已拒绝；失败不改变输出视图。 */
    for (size_t i = 0U; i < 2U; i++) {
        root = base_request();
        cJSON *number = cJSON_GetObjectItemCaseSensitive(root, "algorithmIndex");
        number->valuedouble = i == 0U ? NAN : INFINITY;
        uint32_t index = 123U;
        CHECK(pc_test_start_read_index(root, &index) != PC_TEST_JSON_OK && index == 123U);
        memset(&view, 0xA5, sizeof(view));
        pc_test_start_view_t before = view;
        CHECK(pc_test_start_parse(root, &view) != PC_TEST_JSON_OK && memcmp(&view, &before, sizeof(view)) == 0);
        cJSON_Delete(root);
        root = base_request();
        replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "vdd2", cJSON_CreateNumber(1100));
        number = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "power"), "vdd2");
        number->valuedouble = i == 0U ? NAN : INFINITY;
        CHECK(pc_test_start_parse(root, &view) != PC_TEST_JSON_OK);
        cJSON_Delete(root);
    }
    root = base_request(); cJSON_DeleteItemFromObjectCaseSensitive(root, "power");
    check_request(root, "invalidArgument", 1, 1U);
    /* 0601复用真实分发和JSON组包，证明20mΩ/10mΩ系数进入线上电流/功率。 */
    CHECK(PAC1934_RSENSE_B1_UOHM == PC_TEST_EXPECTED_RSENSE);
    query_monitor = 1;
    pc_protocol_frame_t query = {
        .command_id = PC_COMMAND_QUERY_POWER_MONITOR, .service_id = 0U,
        .sequence = 888U, .json = (const uint8_t *)"{}", .json_length = 2U
    };
    pc_dispatch_response_t response;
    CHECK(pc_dispatch_handle(&query, 18U, &response) == PC_DISPATCH_RESPONSE_READY);
    CHECK(response.data[10] == 6U && response.data[11] == 2U && response.data[15] == 18U);
    cJSON *body = NULL;
    CHECK(pc_json_parse_object(response.data + 18U, response.length - 18U, &body, NULL) == PC_JSON_OK);
    CHECK(cJSON_GetObjectItemCaseSensitive(body, "sampleAgeMs")->valueint == 50);
    CHECK(cJSON_GetObjectItemCaseSensitive(body, "validMask")->valueint == 3);
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(body, "samplePowerState")->valuestring, "on") == 0);
    const int32_t expected_mv[] = {1800, 1100, 600};
    for (int rail = 0; rail < 3; rail++) {
        CHECK(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(body, "voltageMv"), rail)->valueint == expected_mv[rail]);
        double expected_current = rail == 1 ? (PC_TEST_EXPECTED_RSENSE == 20000U ? 0.076 : 0.152) :
                                             (PC_TEST_EXPECTED_RSENSE == 20000U ? 2500.0 : 5000.0);
        CHECK(fabs(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(body, "currentMa"), rail)->valuedouble -
                   expected_current) < 1e-12);
        CHECK(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(body, "powerMw"), rail)->valueint ==
              (PC_TEST_EXPECTED_RSENSE == 20000U ? 80000 : 160000));
    }
    cJSON_Delete(body);
    message("PASS: actual START_TEST parser/dispatch, index and zero hardware side effects\n");
    message("PASS: actual PAC conversion with board shunt reaches 0602 JSON\n");
    finish(0U);
}
