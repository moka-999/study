/* QEMU执行正式PC分发、RK业务及命令表；平台队列/FRU状态由本文件控制。 */
#include "pc_test_service.h"
#include "pc_tester_info.h"
#include "monitor_task.h"
#include "result_led.h"
#include "rk_link.h"
#include "pc_power_service.h"
#include "b2_service.h"
#include <stdio.h>
#include <string.h>
#if defined(TEST_RK_DDR_CAPACITY)
#include "rk_ddr_capacity.h"
#endif
#if defined(PC_COMMAND_CHIP_INFO)
#include "board_rk_config.h"
#endif
#if defined(TEST_B1_AUTO_SLT)
#include "handoff_test.h"
#endif
#if defined(TEST_B1_PROTOCOL_TRACE)
#include "pc_trace.h"
#include "protocol_trace.h"
#include "trace_capture.h"
#endif
#if defined(TEST_B1_CONSOLE_POWER)
#include "log_console.h"
static char console_debug_line[32], console_debug_reply[256];
static char console_func_name[LOG_CONSOLE_FUNC_NAME_SIZE], console_func_reply[LOG_CONSOLE_RECORD_SIZE];
static unsigned console_func_completions;
int log_console_take_func(char name[LOG_CONSOLE_FUNC_NAME_SIZE])
{
    if (!console_func_name[0]) return 0;
    strcpy(name, console_func_name);
    console_func_name[0] = '\0';
    return 1;
}
void log_console_complete_func(const char *text)
{
    snprintf(console_func_reply, sizeof(console_func_reply), "%s", text);
    ++console_func_completions;
}
void log_console_service_debug(log_console_debug_handler_t handler)
{
    if (console_debug_line[0]) {
        handler(console_debug_line, console_debug_reply, sizeof(console_debug_reply));
        console_debug_line[0] = '\0';
    }
}
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
static int console_force_request;
static unsigned console_force_completions;
static const char *console_force_error;
int log_console_take_force_off(void)
{
    int pending = console_force_request;
    console_force_request = 0;
    return pending;
}
void log_console_complete_force_off(const char *error)
{ ++console_force_completions; console_force_error = error; }
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
#ifdef PC_COMMAND_SITE_ENABLE
/* 模拟 B2 已分配给测试仪 2 的站点；范围和目标匹配由正式 010C 代码执行。 */
uint8_t b2_service_tester_id(void) { return b2_site ? 2U : 0U; }
static uint32_t visible_leds = (1U << RESULT_LED_TARGET_COUNT) - 1U;
static uint8_t site_ready_led = 1U;
/* 控制台实际收包和拥塞撤销由UART套件覆盖；此处只验证业务和灯的输出。 */
void log_console_set_site_enabled(uint8_t enabled) { (void)enabled; }
void result_led_set_site(uint8_t enabled, uint32_t visible_mask)
{
    site_ready_led = enabled;
    visible_leds = visible_mask;
    if (!enabled) memset(led_states, 0, sizeof(led_states));
}
#endif
static char site_label[16] = "SITE-7";
static rk_request_t queued_request;
static rk_response_t queued_response;
#if defined(PC_COMMAND_STOP_TEST)
#include "rk_task_control.h"
/* 接收窗口/令牌由RK任务回归覆盖；本套件控制业务快照读到的最新百分比。 */
static int mock_progress = -1;
int rk_link_progress_percent(uint32_t cookie, uint16_t command)
{
    return cookie == queued_request.cookie && command == queued_request.command_id ? mock_progress : -1;
}
/* 记录实际电源编排对 RK 回读的通知；四通道应答和超时由 RK 任务回归验证。 */
static unsigned readback_arms, readback_settings_done, readback_waits;
static int readback_deferred, readback_held, console_spl_ready;
int rk_timing_startup_ready(void) { return console_spl_ready; }
static uint16_t debug_set_command;
static uint32_t debug_set_value;
int rk_task_set_pending(void) { return debug_set_command != 0U; }
const char *rk_task_set_timing(uint16_t command, uint32_t value)
{
    if (mock_power_state != PWR_CTRL_STATE_ON) return "powerOff";
    if (debug_set_command) return "busy";
    debug_set_command = command;
    debug_set_value = value;
    return NULL;
}
void rk_timing_readback_arm(int defer_until_settings)
{
    ++readback_arms;
    readback_deferred = defer_until_settings;
}
void rk_timing_readback_settings_done(void) { ++readback_settings_done; }
int rk_timing_readback_busy(void)
{
    if (readback_held) ++readback_waits;
    return readback_held;
}
/* PC层只注入RK清理确认；真正队列/IDD/串口停止另由RK和交接实际C回归验证。 */
static int stop_requested, stop_ready;
void rk_task_stop_begin(void) { stop_requested = 1; stop_ready = 0; }
int rk_task_stop_ready(void) { return stop_ready; }
void rk_task_stop_end(void) { stop_requested = 0; }
#endif
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
uint32_t HAL_GetTick(void) { return mock_tick; }
/* PC服务测试只验证模拟采样事件的调用合同；IDD适配全流程由RK实际C回归验证。 */
void monitor_publish_sample(const memory_monitor_sample_t *sample, uint8_t groups,
                             uint32_t begin_ms, uint32_t end_ms)
{ (void)sample; (void)groups; (void)begin_ms; (void)end_ms; }
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
    CHECK(power_fixture && now == mock_tick);
    ++power_off_calls;
    if (power_failure != PWR_CTRL_OK) return power_failure;
    mock_power_state = mock_power_state == PWR_CTRL_STATE_ON
        ? PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF : PWR_CTRL_STATE_OFF;
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
    if (!cJSON_IsNumber(value) || value->valuedouble != (double)expected) {
        char diagnostic[160];
        snprintf(diagnostic, sizeof(diagnostic), "field=%s expected=%u actual=%.6f\n",
                 name, (unsigned)expected, cJSON_IsNumber(value) ? value->valuedouble : -1.0);
        message(diagnostic);
    }
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
#if defined(TEST_RK_DDR_CAPACITY)
static void observe_ddrbin_boot(unsigned channel_mib)
{
    static const uint8_t banner[] = "LPDDR4X, 2112MHz\n";
    rk_ddr_capacity_observe(true, banner, sizeof(banner) - 1U);
    for (unsigned channel = 0U; channel < 4U; ++channel) {
        char line[48];
        int length = snprintf(line, sizeof(line), "channel[%u] BW=16 Size=%uMB\n", channel, channel_mib);
        rk_ddr_capacity_observe(true, (const uint8_t *)line, length);
    }
}
#endif

#if defined(PC_COMMAND_CHIP_INFO)
static void check_chip_report(uint32_t sequence, const char *type0, unsigned size0,
                              const char *type1, unsigned size1)
{
    pc_dispatch_response_t response;
    CHECK(pc_dispatch_next_response(sequence, &response) == PC_DISPATCH_RESPONSE_READY);
    cJSON *body = decode(&response, PC_COMMAND_CHIP_INFO, sequence);
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(body, "chipinfo");
    CHECK(cJSON_GetArraySize(body) == 1 && cJSON_IsArray(array) && cJSON_GetArraySize(array) == 2);
    for (unsigned i = 0U; i < 2U; ++i) {
        const cJSON *entry = cJSON_GetArrayItem(array, i);
        CHECK(cJSON_IsObject(entry) && cJSON_GetArraySize(entry) == 3);
        number_is(entry, "mirrorIndex", i);
        string_is(entry, "MemoryType", i ? type1 : type0);
        number_is(entry, "MemorySize", i ? size1 : size0);
    }
    cJSON_Delete(body);
}

/* 只测后续业务的用例从本轮已上报010D的状态开始；首项交错及发送失败单独从原始0106执行。 */
static void deliver_round_chip_info(unsigned channel_mib)
{
    observe_ddrbin_boot(channel_mib);
    check_chip_report(60U, "LPDDR4X", channel_mib * 2U, "LPDDR4X", channel_mib * 2U);
    pc_dispatch_response_sent(PC_COMMAND_CHIP_INFO);
}
#endif

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
#if defined(PC_COMMAND_CHIP_INFO)
        if (strcmp(status->valuestring, "completed") == 0) deliver_round_chip_info(1024U);
#endif
        cJSON_Delete(body);
    }
    unsigned before = queue_calls;
    CHECK(dispatch(PC_COMMAND_START_TEST, json, 17U, &response) == PC_DISPATCH_RESPONSE_READY);
    status_is(&response, error, 17U);
    if (error && strcmp(error, "busy") != 0) CHECK(queue_calls == before);
}
/* 反序构造真实RK帧：FAIL按mask列出颗粒（地址可为0），PASS显式列两颗零地址。 */
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
        if (failed && (mask & (1U << die)) == 0U) continue;
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

#if defined(PC_POWER_SEQUENCE_TEST) || defined(PC_COMMAND_CHIP_INFO)
/* 下面的短等待只为故障注入，绝不是B1获批的硬件时序。 */
static const pc_power_config_t sequence_config = {
    .transition_timeout_ms = 100U, .ready_timeout_ms = 100U,
    .off_hold_ms = 10U, .boot_settle_ms = 5U,
};
#endif

#if defined(PC_COMMAND_CHIP_INFO)
/* 外部启动夹具只模拟硬件，0107仍要运行正式电源服务；在OFF时配置后模拟外部上电。 */
static void configure_external_power(void)
{
    power_fixture = 1;
    mock_power_state = PWR_CTRL_STATE_OFF;
    CHECK(pc_power_service_configure(&sequence_config, NULL) == 0);
    mock_power_state = PWR_CTRL_STATE_ON;
}

/* 实际业务和电源状态机必须先发起并完成OFF；只替代GPIO轨到位和RK清理通知。 */
static void complete_test_power_off(void)
{
    pc_dispatch_response_t response;
    for (unsigned poll = 0U; poll < 6U && mock_power_state != PWR_CTRL_STATE_OFF; ++poll) {
        CHECK(pc_dispatch_next_response(104U, &response) == PC_DISPATCH_NO_RESPONSE);
        pc_dispatch_poll();
        if (mock_power_state == PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF) {
            CHECK(pc_dispatch_next_response(104U, &response) == PC_DISPATCH_NO_RESPONSE);
            mock_power_state = PWR_CTRL_STATE_OFF;
        }
    }
    CHECK(mock_power_state == PWR_CTRL_STATE_OFF);
    if (stop_requested) {
        handoff_test.reserved = 0;
        response_ready = 0;
        stop_ready = 1;
    }
    pc_dispatch_poll();
}

#endif

static void delivery_retry_check(void)
{
#if defined(PC_COMMAND_CHIP_INFO)
    configure_external_power();
#endif
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
#if defined(PC_COMMAND_CHIP_INFO)
        mock_power_state = PWR_CTRL_STATE_ON;
#endif
        start_expect(START_JSON, NULL);
        inject(RK_RESPONSE_COMPLETE, 0U);
        pc_json_configure_allocator(no_memory, free);
        CHECK(pc_dispatch_next_response(35U, &response) == 0 && pc_test_service_busy());
        pc_json_configure_allocator(NULL, NULL);
        if (disconnect) pc_dispatch_disconnected();
        else {
#if defined(PC_COMMAND_CHIP_INFO)
            CHECK(pc_test_round_end(0U, 36U, &response) == PC_DISPATCH_NO_RESPONSE);
            complete_test_power_off();
            CHECK(pc_dispatch_next_response(36U, &response) == PC_DISPATCH_RESPONSE_READY);
            cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 36U);
            number_is(body, "requestCommandId", PC_COMMAND_ROUND_END);
            string_is(body, "status", "completed");
            cJSON_Delete(body);
#else
            CHECK(pc_test_round_end(0U, 36U, &response) == PC_DISPATCH_RESPONSE_READY);
#endif
        }
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
               299.999) < 1e-9);
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
#if defined(TEST_B1_AUTO_SLT)
    handoff_test_reset();
#endif
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

#if defined(PC_COMMAND_CHIP_INFO)
/* RK没有开始执行算法；两颗都应报告启动失败，且没有错误地址和采样值。 */
static void training_failed_result(int para, uint32_t elapsed_ms)
{
    pc_dispatch_response_t response;
    unsigned off = power_off_calls;
    complete_test_power_off();
    CHECK(power_off_calls == off + 1U);
    /* 第一次轮询可先消费电源终态；后续轮询交付结果，短暂缺内存时不能丢失。 */
    pc_json_configure_allocator(no_memory, free);
    for (unsigned attempt = 0U; attempt < 2U; ++attempt)
        CHECK(pc_dispatch_next_response(104U, &response) != PC_DISPATCH_RESPONSE_READY);
    pc_json_configure_allocator(NULL, NULL);
    CHECK(pc_test_service_busy() && pc_dispatch_next_response(104U, &response) == PC_DISPATCH_RESPONSE_READY);
    cJSON *body = decode(&response, para ? PC_COMMAND_PARA_RESULT : PC_COMMAND_TEST_RESULT_REPORT, 104U);
    string_is(body, "algorithmType", para ? "parametric" : "functional");
    string_is(body, "algorithmName", para ? "idd_0" : "mats");
    number_is(body, "algorithmIndex", para ? 91U : 37U);
    number_is(body, "testTimeMs", elapsed_ms);
    string_is(body, "siteId", site_label);
    const cJSON *measurement = cJSON_GetObjectItemCaseSensitive(body, "measurementData");
    CHECK(cJSON_IsObject(measurement) && cJSON_GetArraySize(measurement) == 0);
    const cJSON *results = cJSON_GetObjectItemCaseSensitive(body, "testResults");
    CHECK(cJSON_IsArray(results) && cJSON_GetArraySize(results) == 2);
    for (unsigned die = 0U; die < 2U; ++die) {
        const cJSON *entry = cJSON_GetArrayItem(results, die);
        number_is(entry, "mirrorIndex", die);
        number_is(entry, "testStatus", 0U);
        string_is(entry, "physicalAddr", "");
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(entry, "resultDesc");
        string_is(desc, "errorType", "DdrTrainingFailed");
        CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(desc, "firstErrorAddress")));
    }
    cJSON_Delete(body);
    CHECK(!pc_test_service_busy() && !pc_power_service_busy());
    CHECK(pc_dispatch_next_response(105U, &response) == PC_DISPATCH_NO_RESPONSE);
}
#endif

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
#ifdef PC_COMMAND_SHMOO_START
    char status[256];
    mock_progress = 0;
    pc_test_format_status(status, sizeof(status));
    CHECK(strstr(status, "round=active type=functional algorithm=mats phase=running progress=0%"));
    mock_progress = -1;
#endif
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

    /* B1冷启动未收到心跳，必须先下电再报训练失败，V2仍沿用原超时策略。 */
    configure_power(); start_expect(COLD_START, NULL); cold_to_ready();
    unsigned queued = queue_calls;
    step_power(sequence_config.ready_timeout_ms);
#if defined(PC_COMMAND_CHIP_INFO)
    training_failed_result(0, 0U);
    CHECK(queue_calls == queued && mock_power_state == PWR_CTRL_STATE_OFF);
    start_expect(COLD_CHANGED, NULL);
    cold_to_ready(); ready_starts_algorithm();
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDD1) == 1810U);
    result_is(0U, NULL, "SITE-7");
    queued = queue_calls;
    /* 心跳超时后的下电也可能失败；失败原因必须归属电源操作，不能假报DDR点已收尾。 */
    configure_power(); start_expect(COLD_START, NULL); cold_to_ready();
    step_power(sequence_config.ready_timeout_ms);
    power_failure = PWR_CTRL_ERR_FAULT;
    step_power(0U); preparation_failed("executionFailed");
    CHECK(mock_power_state == PWR_CTRL_STATE_ON && queue_calls == queued);
#else
    preparation_failed("timeout");
    CHECK(queue_calls == queued && mock_power_state == PWR_CTRL_STATE_ON);
    CHECK(pc_power_service_diagnostic()->failed_phase == PC_POWER_WAIT_RK_READY);
#endif
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
static void console_cold_finish(ddr_memory_type_t memory_type)
{
    unsigned completed = console_completions, on = power_on_calls, arms = rk_arms, queued = queue_calls;
    unsigned previous_readback = readback_arms;
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_OFF_HOLD);
    step_power(sequence_config.off_hold_ms);
    CHECK(last_boot_mode == BOOT_MODE_FSPI_M1 && power_on_calls == on);
    step_power(sequence_config.boot_settle_ms);
    CHECK(power_on_calls == on + 1U && mock_power_state == PWR_CTRL_STATE_WAIT_FIRST_RAIL);
    CHECK(ddr_power_get_memory_type() == memory_type);
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDD1) == 1800U);
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDD2) == 1100U);
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDDQ) == (memory_type == DDR_MEMORY_LPDDR4 ? 1100U : 600U));
    CHECK(console_completions == completed);
    mock_power_state = PWR_CTRL_STATE_ON; step_power(1U);
    CHECK(console_completions == completed + 1U && console_error == NULL);
    CHECK(!pc_power_service_busy() && !pc_test_service_busy());
    CHECK(readback_arms == previous_readback + 1U && !readback_deferred);
    CHECK(rk_arms == arms && queue_calls == queued); /* 不等待SPL心跳、不发算法或建SLT占用。 */
}
static void console_power_check(void)
{
    pc_dispatch_response_t response;
    configure_power();
    /* 边界和语法错误从控制台原文走正式解析，不能截断成可用数值。 */
    const char *invalid[] = {"set freq -2", "set freq 9223372036854775808", "set tWR -1",
                             "set tWR 4294967296", "set tWR 2.5", "set BL 16x", "set Freq"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        strcpy(console_debug_line, invalid[i]);
        pc_dispatch_poll();
        CHECK(strstr(console_debug_reply, "invalidArgument"));
    }
    strcpy(console_debug_line, "set freq -1"); pc_dispatch_poll();
    CHECK(strstr(console_debug_reply, "-1 next manual start"));
    strcpy(console_debug_line, "test status"); pc_dispatch_poll();
    CHECK(strstr(console_debug_reply, "round="));
    CHECK(pc_dispatch_debug_set("Freq", 1066) == NULL);
    CHECK(pc_power_service_configure(NULL, NULL) == 0);
    console_command(LOG_CONSOLE_POWER_START_RK);
    CHECK(strcmp(console_error, "notConfigured") == 0 && !pc_power_service_busy());

    configure_power();
    const uint16_t biased[] = {1700U, 1060U, 570U};
    /* PC冷启动由自己的令牌控制，控制台暂存只由下一次成功受理的start rk消费。 */
    power_command(PC_COMMAND_POWER_ON, "accepted", NULL);
    step_power(10U); step_power(5U);
    CHECK(rk_link_frequency_get(NULL) == -1);
    mock_power_state = PWR_CTRL_STATE_ON; step_power(1U);
    power_completion(PC_COMMAND_POWER_ON, NULL);
    console_command(LOG_CONSOLE_POWER_END_RK); step_power(0U);
    mock_power_state = PWR_CTRL_STATE_OFF; step_power(1U);
    CHECK(ddr_power_configure_training(DDR_MEMORY_LPDDR4X, biased) == IF_OK);
    CHECK(pc_power_service_configure(&sequence_config, NULL) == 0);
    console_command(LOG_CONSOLE_POWER_START_RK);
    power_command(PC_COMMAND_POWER_OFF, "failed", "busy");
    start_expect(START_JSON, "busy");
    pc_dispatch_disconnected(); /* 无PC/断线均不取消已经受理的本地动作。 */
    console_cold_finish(DDR_MEMORY_LPDDR4X);
    CHECK(rk_link_frequency_get(NULL) == 1066);
    CHECK(pc_dispatch_debug_set("tRCD", 35) == NULL && debug_set_command == 0x2BU && debug_set_value == 35U);
    start_expect(START_JSON, "busy");
    debug_set_command = 0U; /* RK实际回读取值及超时由协议任务回归覆盖。 */
    CHECK(pc_dispatch_next_response(500U, &response) == 0);

    /* 已经ON也重新下电，确保Boot和训练目标真正重新进入启动流程。 */
    console_command(LOG_CONSOLE_POWER_START_LPDDR4);
    direct_off_to_hold(); console_cold_finish(DDR_MEMORY_LPDDR4);
    CHECK(rk_link_frequency_get(NULL) == -1);
    console_command(LOG_CONSOLE_POWER_START_RK);
    direct_off_to_hold(); console_cold_finish(DDR_MEMORY_LPDDR4X);
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
/* PC可传index=0；后续受理、准备与收尾响应仍按原合同使用null。 */
#define SLT_START "{\"algorithmType\":\"slt\",\"algorithmName\":\"memtest86\",\"algorithmIndex\":0,\"power\":{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1800\",\"vdd2\":\"1100\",\"vddq\":\"600\"},\"reboot\":false,\"bootMode\":\"emmc\",\"TimingParameters\":{}}"
#define SLT_RESULT "{\"siteId\":\"SITE-7\",\"algorithmType\":\"slt\",\"algorithmName\":\"memtest86\",\"algorithmIndex\":null,\"testTimeMs\":null,\"testResults\":[{\"mirrorIndex\":0,\"testStatus\":0,\"resultDesc\":null,\"physicalAddr\":\"\"},{\"mirrorIndex\":1,\"testStatus\":1,\"resultDesc\":null,\"physicalAddr\":\"\"}],\"measurementData\":null}"
#if defined(TEST_B1_AUTO_SLT)
#define SLT_LEGACY_STATE_ERROR "notSupported"
#define SLT_LEGACY_ARGUMENT_ERROR "notSupported"
#else
/* V2尚无EDK交接，仍校验旧010B的身份与结果结构。 */
#define SLT_LEGACY_STATE_ERROR "invalidState"
#define SLT_LEGACY_ARGUMENT_ERROR "invalidArgument"
#endif

static void round_command(uint16_t command, const char *json, const char *status, const char *error)
{
    pc_dispatch_response_t response;
    int result = dispatch(command, json, 77U, &response);
#if defined(PC_COMMAND_CHIP_INFO)
    if (command == PC_COMMAND_ROUND_END && !error) {
        if (result == PC_DISPATCH_NO_RESPONSE) {
            complete_test_power_off();
            result = pc_dispatch_next_response(77U, &response);
        }
        CHECK(mock_power_state == PWR_CTRL_STATE_OFF && !pc_test_service_busy());
    }
#endif
    CHECK(result == PC_DISPATCH_RESPONSE_READY);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 77U);
    number_is(body, "requestCommandId", command);
    string_is(body, "status", status);
    if (error) string_is(body, "errorCode", error);
    else CHECK(!cJSON_GetObjectItemCaseSensitive(body, "errorCode"));
    if (command == PC_COMMAND_SLT_START || command == PC_COMMAND_SLT_RESULT)
        CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(body, "algorithmIndex")));
    cJSON_Delete(body);
}

static void round_begin(void)
{
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "completed", NULL);
#if defined(PC_COMMAND_CHIP_INFO)
    deliver_round_chip_info(1024U);
#endif
}
static void round_end(void)
{
    round_command(PC_COMMAND_ROUND_END, "{\"failChips\":[]}", "completed", NULL);
}

#if defined(TEST_RK_DDR_CAPACITY)
static void check_round_capacity(bool active, uint32_t current, uint32_t last)
{
    rk_ddr_capacity_t capacity = rk_ddr_capacity_snapshot();
    CHECK(capacity.round_active == active);
    CHECK(capacity.current_mib == current && capacity.last_completed_mib == last);
    CHECK(capacity.has_completed);
}
#endif

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
#if defined(PC_COMMAND_CHIP_INFO)
    mock_power_state = PWR_CTRL_STATE_ON; /* 本组先验证复用已上电RK的PARA入口。 */
#endif
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
        mock_tick += 5678U;
        inject_mask(RK_RESPONSE_COMPLETE, mask, UINT64_C(0x1234), UINT64_C(0x9876));
        for (unsigned rail = 0U; rail < 3U; ++rail) {
            queued_response.measurement.current[rail] = (idd_metric_stats_t){
                .sum = 9003 + rail, .minimum = 1001, .maximum = 5001, .valid_count = 3};
            queued_response.measurement.voltage[rail] = (idd_metric_stats_t){
                .sum = 3600, .mean = 1800, .minimum = 1790, .maximum = 1810, .valid_count = 2};
        }
        CHECK(pc_dispatch_next_response(62U, &response) == 1 && !pc_test_service_busy());
        cJSON *body = decode(&response, PC_COMMAND_PARA_RESULT, 62U);
        string_is(body, "algorithmType", "parametric");
        string_is(body, "algorithmName", names[i]);
        string_is(body, "siteId", site_label);
        number_is(body, "algorithmIndex", 100U + i);
        number_is(body, "testTimeMs", 5678U);
        const cJSON *measurement = cJSON_GetObjectItemCaseSensitive(body, "measurementData");
        CHECK(cJSON_GetArraySize(measurement) == 18);
        CHECK(fabs(cJSON_GetObjectItemCaseSensitive(measurement, "Idd1Ave")->valuedouble - 3.001) < 1e-9);
        CHECK(fabs(cJSON_GetObjectItemCaseSensitive(measurement, "Idd2Ave")->valuedouble - 3.001) < 1e-9);
        CHECK(fabs(cJSON_GetObjectItemCaseSensitive(measurement, "IddqAve")->valuedouble - 3.002) < 1e-9);
        number_is(measurement, "Vdd1Ave", 1800U);
        CHECK(cJSON_GetObjectItemCaseSensitive(body, "measurementData"));
        const cJSON *chips = cJSON_GetObjectItemCaseSensitive(body, "testResults");
        CHECK(cJSON_GetArraySize(chips) == 2);
        for (unsigned die = 0U; die < 2U; ++die) {
            const cJSON *chip = cJSON_GetArrayItem(chips, die);
            number_is(chip, "mirrorIndex", die);
            number_is(chip, "testStatus", 1U);
            CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(chip, "resultDesc")));
            string_is(chip, "physicalAddr", "");
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
#if defined(PC_COMMAND_CHIP_INFO)
    uint32_t accepted_ms = mock_tick;
    para_command(PC_COMMAND_PARA_START, json, 91U, NULL);
    cold_to_ready();
    step_power(sequence_config.ready_timeout_ms - 1U);
    CHECK(pc_dispatch_next_response(62U, &response) == PC_DISPATCH_NO_RESPONSE);
    step_power(1U);
    training_failed_result(1, mock_tick - accepted_ms);
    CHECK(queue_calls == queued);
#endif
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
#ifdef PC_COMMAND_SHMOO_START
    char status[256];
    mock_progress = 88;
    pc_test_format_status(status, sizeof(status));
    CHECK(strstr(status, "type=parametric algorithm=idd_0 phase=running progress=--"));
    mock_progress = -1;
#endif
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
    /* 断线等待在途算法；B1提前0107则等RK掉电清理，再解除占用。 */
    para_command(PC_COMMAND_PARA_START, json, 91U, NULL);
    pc_dispatch_disconnected(); CHECK(pc_test_service_busy());
    para_command(PC_COMMAND_PARA_START, json, 91U, "busy");
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(65U, &response) == 0 && !pc_test_service_busy());
    round_begin();
    para_command(PC_COMMAND_PARA_START, json, 91U, NULL);
    round_end();
#if defined(PC_COMMAND_CHIP_INFO)
    CHECK(!pc_test_service_busy() && mock_power_state == PWR_CTRL_STATE_OFF);
#else
    CHECK(pc_test_service_busy());
#endif
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
#if defined(PC_COMMAND_SITE_ENABLE)
    const result_led_state_t emcp_pass = RESULT_LED_OFF, emcp_fail = RESULT_LED_OFF;
#else
    const result_led_state_t emcp_pass = RESULT_LED_PASS, emcp_fail = RESULT_LED_FAIL;
#endif
#if defined(PC_COMMAND_CHIP_INFO)
    configure_external_power();
#elif defined(B1_RK_UART)
    mock_power_state = PWR_CTRL_STATE_ON;
#endif
    round_command(PC_COMMAND_START_TEST, START_JSON, "failed", "invalidState");
    round_command(PC_COMMAND_ROUND_END, "{\"failChips\":[]}", "failed", "invalidState");
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
#if defined(TEST_RK_DDR_CAPACITY)
    rk_ddr_capacity_observe(false, NULL, 0);
    observe_ddrbin_boot(1024U);
#endif
#if defined(PC_COMMAND_SITE_ENABLE)
    for (unsigned i = 0U; i < RESULT_LED_TARGET_COUNT; ++i) CHECK(led_states[i] == RESULT_LED_OFF);
#else
    for (unsigned i = 0U; i < RESULT_LED_TARGET_COUNT; ++i) CHECK(led_states[i] == RESULT_LED_TEST_NORMAL);
#endif
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "failed", "busy");
    const char *bad_lists[] = {
        "{}", "{\"failChips\":null}", "{\"failChips\":{},\"leds\":{}}",
        "{\"errorchip\":[]}", "{\"failchips\":[]}",
        "{\"failChips\":[],\"errorchip\":[]}", "{\"failChips\":[],\"failChips\":[]}",
        "{\"failChips\":[{\"physicalAddr\":\"a\",\"mirrorIndex\":0},{\"physicalAddr\":\"b\",\"mirrorIndex\":3}]}",
        "{\"failChips\":[{\"physicalAddr\":\"\",\"mirrorIndex\":-1}]}",
        "{\"failChips\":[{\"physicalAddr\":\"\",\"mirrorIndex\":1.5}]}",
        "{\"failChips\":[{\"physicalAddr\":\"\",\"mirrorindex\":1}]}",
        "{\"failChips\":[{\"physicalAddr\":3,\"mirrorIndex\":1}]}",
        "{\"failChips\":[{\"physicalAddr\":\"\",\"mirrorIndex\":\"1\"}]}",
        "{\"failChips\":[{\"physicalAddr\":\"\",\"mirrorIndex\":0,\"mirrorIndex\":1}]}"
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
    round_command(PC_COMMAND_ROUND_END, "{\"failChips\":[]}", "failed", "executionFailed");
    led_failure = 0;
    /* 同镜像多颗、重复芯片都只归并到一盏FAIL灯，不要求PC发送固定三灯对象。 */
    round_command(PC_COMMAND_ROUND_END,
        "{\"failChips\":[{\"physicalAddr\":\"a\",\"mirrorIndex\":0},{\"physicalAddr\":\"b\",\"mirrorIndex\":0},{\"physicalAddr\":\"b\",\"mirrorIndex\":0},{\"physicalAddr\":\"\",\"mirrorIndex\":2}]}",
        "completed", NULL);
#if defined(TEST_RK_DDR_CAPACITY)
    /* 重复 0106、非法 0107、分配或灯态失败均不能提前覆盖这份值。 */
    check_round_capacity(false, 0U, 4096U);
#endif
    CHECK(led_states[0] == RESULT_LED_FAIL && led_states[1] == RESULT_LED_PASS && led_states[2] == emcp_fail);
    round_command(PC_COMMAND_ROUND_END, "{\"failChips\":[]}", "failed", "invalidState");
    /* 同一镜像两条地址只点亮该镜像 FAIL，另一颗 DDR 显示 PASS。 */
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "completed", NULL);
    round_command(PC_COMMAND_ROUND_END,
        "{\"failChips\":[{\"physicalAddr\":\"0x0000000000200000\",\"mirrorIndex\":0},{\"physicalAddr\":\"0x0000000000200320\",\"mirrorIndex\":0}]}",
        "completed", NULL);
    CHECK(led_states[0] == RESULT_LED_FAIL && led_states[1] == RESULT_LED_PASS && led_states[2] == emcp_pass);
#if defined(PC_COMMAND_SITE_ENABLE)
    b2_known = 0U;
#endif
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "completed", NULL);
    round_end();
    CHECK(led_states[0] == RESULT_LED_PASS && led_states[1] == RESULT_LED_PASS && led_states[2] == emcp_pass);
#if defined(TEST_RK_DDR_CAPACITY)
    check_round_capacity(false, 0U, 0U);
#endif
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "completed", NULL);
#if defined(PC_COMMAND_CHIP_INFO)
    mock_power_state = PWR_CTRL_STATE_ON;
#endif
    round_command(PC_COMMAND_START_TEST, START_JSON, "accepted", NULL);
#if defined(TEST_RK_DDR_CAPACITY)
    check_round_capacity(true, 4096U, 0U); /* 实际受理了沿用已运行 RK 的算法。 */
#endif
#if defined(PC_COMMAND_CHIP_INFO)
    check_chip_report(60U, "LPDDR4X", 2048U, "LPDDR4X", 2048U);
    pc_dispatch_response_sent(PC_COMMAND_CHIP_INFO);
    CHECK(pc_dispatch_next_response(61U, &response) == 0);
#endif
#if defined(PC_COMMAND_CHIP_INFO)
    CHECK(dispatch(PC_COMMAND_ROUND_END,
        "{\"failChips\":[{\"mirrorIndex\":1,\"physicalAddr\":\"\"}]}", 77U, &response) == PC_DISPATCH_NO_RESPONSE);
    /* 0107已经提交结果，等RK下电期间的高温通知和离线清除都不能再改灯。 */
    const uint8_t hot[] = {1U, 1U, 1U}, clear_modes[] = {0U, 0U, 0U};
    writes = led_writes;
    CHECK(pc_test_round_update_modes(7U, hot) && pc_test_round_update_modes(7U, clear_modes));
    CHECK(led_writes == writes && led_states[0] == RESULT_LED_PASS &&
          led_states[1] == RESULT_LED_FAIL && led_states[2] == RESULT_LED_OFF);
#else
    round_end();
#endif
    writes = led_writes;
    CHECK(pc_test_service_busy());
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "failed", "busy");
    round_command(PC_COMMAND_POWER_OFF, "{}", "failed", "busy");
    round_command(PC_COMMAND_FIRMWARE_UPGRADE_REQUEST, "{\"action\":\"mcu_begin\"}", "failed", "notSupported");
    CHECK(dispatch(PC_COMMAND_CLIENT_HEARTBEAT, "{}", 77U, &response) == 1);
    inject(RK_RESPONSE_COMPLETE, UINT64_C(0x123456789ABCDEF0));
#if defined(PC_COMMAND_CHIP_INFO)
    complete_test_power_off();
    CHECK(pc_dispatch_next_response(77U, &response) == PC_DISPATCH_RESPONSE_READY);
    cJSON *end = decode(&response, PC_COMMAND_COMMAND_RESP, 77U);
    number_is(end, "requestCommandId", PC_COMMAND_ROUND_END);
    string_is(end, "status", "completed");
    cJSON_Delete(end);
#endif
    CHECK(pc_dispatch_next_response(78U, &response) == 0 && !pc_test_service_busy());
    CHECK(led_writes == writes);
    round_command(PC_COMMAND_START_TEST, START_JSON, "failed", "invalidState");
    round_begin();
#if defined(PC_COMMAND_CHIP_INFO)
    mock_power_state = PWR_CTRL_STATE_ON;
#endif
    round_command(PC_COMMAND_START_TEST, START_JSON, "accepted", NULL);
    inject(RK_RESPONSE_TIMEOUT, 0U);
    CHECK(pc_dispatch_next_response(78U, &response) == 1);
    round_end();
#if defined(PC_COMMAND_CHIP_INFO)
    CHECK(!pc_test_service_busy());
#else
    CHECK(pc_test_service_busy());
#endif
    inject(RK_RESPONSE_RECOVERED, 0U);
    CHECK(pc_dispatch_next_response(79U, &response) == 0 && !pc_test_service_busy());
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "completed", NULL);
#if defined(TEST_RK_DDR_CAPACITY)
    observe_ddrbin_boot(2048U);
    check_round_capacity(true, 8192U, 4096U);
#endif
    pc_dispatch_disconnected();
#if defined(TEST_RK_DDR_CAPACITY)
    check_round_capacity(false, 0U, 4096U);
#endif
    round_command(PC_COMMAND_START_TEST, START_JSON, "failed", "invalidState");
    round_begin(); round_end();
    /* B2模式独立于轮次保存：未知灭灯、局部更新，结束后不能覆盖PASS/FAIL。 */
    b2_known = 1U; b2_modes[0] = 1U;
#if defined(PC_COMMAND_SITE_ENABLE)
    /* B2 已将 eMCP 标为高温，B1 开始 DDR 整轮时仍要保持该灯熄灭。 */
    b2_known |= 4U;
    b2_modes[2] = 1U;
#endif
    round_begin();
    CHECK(led_states[0] == RESULT_LED_TEST_HOT && led_states[1] == RESULT_LED_OFF &&
          led_states[2] == RESULT_LED_OFF);
    const uint8_t update[] = {0U, 1U, 0U};
#if defined(PC_COMMAND_SITE_ENABLE)
    writes = led_writes;
    CHECK(pc_test_round_update_modes(4U, hot) && led_writes == writes);
    CHECK(pc_test_round_update_modes(6U, hot));
#else
    CHECK(pc_test_round_update_modes(2U, update));
#endif
    CHECK(led_states[0] == RESULT_LED_TEST_HOT && led_states[1] == RESULT_LED_TEST_HOT &&
          led_states[2] == RESULT_LED_OFF);
#if defined(PC_COMMAND_SITE_ENABLE)
    CHECK(pc_test_round_update_modes(1U, update));
    CHECK(led_states[0] == RESULT_LED_OFF && led_states[1] == RESULT_LED_TEST_HOT);
    CHECK(pc_test_round_update_modes(7U, clear_modes));
    CHECK(led_states[0] == RESULT_LED_OFF && led_states[1] == RESULT_LED_OFF &&
          led_states[2] == RESULT_LED_OFF);
    CHECK(pc_test_round_update_modes(2U, update) && led_states[1] == RESULT_LED_TEST_HOT);
#endif
    writes = led_writes;
    CHECK(!pc_test_round_update_modes(8U, update) && led_writes == writes);
    led_failure = 1;
    CHECK(!pc_test_round_update_modes(2U, update) && led_writes == writes);
    led_failure = 0;
    round_end(); writes = led_writes;
    CHECK(pc_test_round_update_modes(1U, update) && led_writes == writes);
    CHECK(led_states[0] == RESULT_LED_PASS);
#if defined(PC_COMMAND_SITE_ENABLE)
    pc_dispatch_disconnected();
    CHECK(led_states[0] == RESULT_LED_PASS && led_writes == writes);
    round_begin();
    CHECK(led_states[0] == RESULT_LED_TEST_HOT);
    pc_dispatch_disconnected();
    CHECK(led_states[0] == RESULT_LED_OFF && led_states[1] == RESULT_LED_OFF &&
          led_states[2] == RESULT_LED_OFF);
#endif
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
#if defined(PC_COMMAND_CHIP_INFO)
    mock_power_state = PWR_CTRL_STATE_ON; /* 这一组从已启动的外部RK读取/写入时序。 */
#endif
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
#if defined(PC_COMMAND_CHIP_INFO)
    mock_power_state = PWR_CTRL_STATE_ON;
#endif
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
#if defined(PC_COMMAND_CHIP_INFO)
    mock_power_state = PWR_CTRL_STATE_ON;
#endif
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
#if defined(TEST_B1_PROTOCOL_TRACE)
        trace_capture_begin();
#endif
        start_expect(json, NULL);
#if defined(TEST_B1_PROTOCOL_TRACE)
        CHECK(strstr(trace_capture_text(), "power=LPDDR4X target_mV=1800/1100/600"));
        CHECK(strstr(trace_capture_text(), run == 2U ? "boot=emmc Freq=1800 timingCount=1" :
                                                     "boot=emmc Freq=1600 timingCount=1"));
        CHECK(strstr(trace_capture_text(), "[power] action=3 phase="));
        CHECK(strstr(trace_capture_text(), " error=none at=idle\r\n"));
        trace_capture_stop();
#endif
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
    /* 复现PC的完整gtest-rand请求：只注入普通心跳及本地发送事件，全程不注入0x35。 */
    const char request[] = "{\"algorithmType\":\"functional\",\"algorithmName\":\"gtest-rand\",\"algorithmIndex\":1,"
        "\"power\":{\"memoryType\":\"LPDDR4X\",\"vdd1\":\"1800\",\"vdd2\":\"1100\",\"vddq\":\"1100\"},"
        "\"TimingParameters\":{\"Freq\":\"2133\",\"CASReadLatency\":\"36\",\"CASWriteLatency\":\"36\","
        "\"tREFI\":\"4\",\"tWR\":\"40\",\"tRTP\":\"16\",\"tRCD\":\"38\",\"tRP\":\"40\","
        "\"tRASmin\":\"81\",\"tRASmax\":\"160\",\"tWTR\":\"20\",\"tRFCmin\":\"729\","
        "\"tFAW\":\"77\",\"tCCD\":\"16\",\"tRRD\":\"16\",\"BL\":\"8\"},\"reboot\":true,\"bootMode\":\"emmc\"}";
    configure_power();
    unsigned before = queue_calls;
    round_command(PC_COMMAND_START_TEST, request, "accepted", NULL);
    CHECK(rk_link_frequency_get(NULL) == 2133);
    cold_to_ready();
    CHECK(pc_dispatch_next_response(105U, &response) == 0 && queue_calls == before);
    confirm_power(1); step_power(0U);
    CHECK(pc_dispatch_next_response(105U, &response) == 0);
    CHECK(queued_request.command_id == 0x25U && queued_request.parameter_value == 36U);
    timing_sent(14U);
    CHECK(queued_request.command_id == 0x34U && queued_request.parameter_value == 8U);
    queued_response = (rk_response_t){.cookie = queued_request.cookie, .status = RK_RESPONSE_SENT};
    response_ready = 1;
    CHECK(pc_dispatch_next_response(105U, &response) == 1);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 105U);
    string_is(body, "status", "completed"); number_is(body, "algorithmIndex", 1U); cJSON_Delete(body);
    CHECK(queue_calls == before + 16U && queued_request.command_id == 2U);
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(106U, &response) == 1 && !pc_test_service_busy());
    body = decode(&response, PC_COMMAND_TEST_RESULT_REPORT, 106U);
    string_is(body, "algorithmName", "gtest-rand"); cJSON_Delete(body);
    before = queue_calls;
    start_expect(json, NULL);
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_REQUEST_OFF);
    uint32_t token; (void)rk_link_frequency_get(&token); rk_link_frequency_note_tx(token, 0);
    direct_off_to_hold(); cold_to_ready(); confirm_power(1); step_power(0U);
    CHECK(pc_dispatch_next_response(105U, &response) == 1);
    body = decode(&response, PC_COMMAND_COMMAND_RESP, 105U);
    string_is(body, "errorCode", "txFailed"); cJSON_Delete(body);
    CHECK(!pc_test_service_busy() && queue_calls == before);
    cJSON_free(json); cJSON_Delete(root); round_end();
    message("PASS: optional 0x35, gtest-rand completed and 0105 without query, cold boot reuse and frequency TX failure\n");
}
#endif

#if defined(PC_POWER_SEQUENCE_TEST) || (defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION)
#if defined(TEST_B1_AUTO_SLT)
/* OFF结束不能代替115200恢复；成功收尾不主动生成PC响应，断线也同样清理。 */
static void slt_finish_off(void)
{
    pc_dispatch_response_t response;
    uint32_t frequency_token;
    int64_t frequency = rk_link_frequency_get(&frequency_token);
    int frequency_status = rk_link_frequency_status(frequency_token);
    CHECK(pc_dispatch_next_response(86U, &response) == 0);
    CHECK(pc_power_service_diagnostic()->phase == PC_POWER_REQUEST_OFF);
    step_power(1U);
    CHECK(pc_dispatch_next_response(86U, &response) == 0);
    CHECK(mock_power_state == PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF);
    mock_power_state = PWR_CTRL_STATE_OFF; step_power(1U);
    CHECK(pc_dispatch_next_response(86U, &response) == 0 && pc_test_service_busy());
    CHECK(!pc_power_service_busy() && handoff_mcu_busy());
    /* 仅OFF完成还未恢复协议口时，仍保留本轮频率，不能提前切回缺省值。 */
    if (frequency_token) {
        uint32_t current_token;
        CHECK(rk_link_frequency_get(&current_token) == frequency && current_token == frequency_token);
        CHECK(rk_link_frequency_status(frequency_token) == frequency_status);
    }
    handoff_test_set_status(HANDOFF_SLT_OFF_READY);
    CHECK(pc_dispatch_next_response(86U, &response) == 0);
    CHECK(pc_dispatch_next_response(87U, &response) == 0 && !pc_test_service_busy());
    uint32_t cleared_token;
    CHECK(rk_link_frequency_get(&cleared_token) == -1 && cleared_token == 0U);
    CHECK(rk_link_frequency_status(frequency_token) == -1);
}

/* 合法旧010B只回notSupported；不能抢走新算法、改变灯态或提前发起关电。 */
static void slt_legacy_rejected(void)
{
    unsigned queued = queue_calls, on = power_on_calls, off = power_off_calls, writes = led_writes;
    int busy = pc_test_service_busy(), power_busy = pc_power_service_busy();
    uint32_t cookie = handoff_test.cookie;
    handoff_slt_status_t status = handoff_test.status;
    const cJSON *timing = pc_test_service_timing_parameters();
    round_command(PC_COMMAND_SLT_RESULT, SLT_RESULT, "failed", "notSupported");
    CHECK(queue_calls == queued && power_on_calls == on && power_off_calls == off && led_writes == writes);
    CHECK(pc_test_service_busy() == busy && pc_power_service_busy() == power_busy);
    CHECK(handoff_test.cookie == cookie && handoff_test.status == status);
    CHECK(pc_test_service_timing_parameters() == timing);
}
#endif

/* B1只根据EDK归还自动收尾；V2尚无handoff，继续验证原010B结束基线。 */
static void slt_finish_session(void)
{
#if defined(TEST_B1_AUTO_SLT)
    CHECK(pc_test_service_busy());
    if (!handoff_test.cancelled) handoff_test_set_status(HANDOFF_SLT_RELEASED);
    slt_finish_off();
#else
    round_command(PC_COMMAND_SLT_RESULT, SLT_RESULT, "completed", NULL);
#endif
}

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
    CHECK(rk_link_frequency_get(NULL) == 1600);
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
    slt_finish_session();
    CHECK(!pc_test_service_busy()); round_end();
    message("PASS: SLT sends timing after SPL heartbeat without a frequency query; B1 EDK release owns completion; V2 keeps its legacy completion\n");
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
#ifdef PC_COMMAND_SHMOO_START
    if (!discarded) {
        char status[256];
        pc_test_format_status(status, sizeof(status));
        CHECK(strstr(status, "type=slt algorithm=memtest86 phase=waitRelease progress=--"));
    }
#endif
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

#if defined(TEST_B1_AUTO_SLT)
/* 仅填Freq允许先回010A completed；后续查询必须仍能读到本轮cookie和2133。 */
static uint32_t slt_frequency_start(void)
{
    cJSON *start = cJSON_Parse(SLT_START); CHECK(start);
    CHECK(cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(start, "TimingParameters"), "Freq", "2133"));
    char *json = cJSON_PrintUnformatted(start); CHECK(json); cJSON_Delete(start);
    round_command(PC_COMMAND_SLT_START, json, "accepted", NULL); cJSON_free(json);
    uint32_t token;
    CHECK(rk_link_frequency_get(&token) == 2133 && token == handoff_test.cookie && token != 0U);
    CHECK(rk_link_frequency_status(token) == 0);
    return token;
}

static void slt_frequency_is(uint32_t token, int status)
{
    uint32_t current;
    CHECK(rk_link_frequency_get(&current) == 2133 && current == token);
    CHECK(rk_link_frequency_status(token) == status);
}

static void slt_prepare_failure(const char *error, int stays_busy)
{
    pc_dispatch_response_t response;
    unsigned queued = queue_calls;
    int reported = 0;
    for (unsigned i = 0U; i < 12U; ++i) {
        if (mock_power_state == PWR_CTRL_STATE_WAIT_FIRST_RAIL) mock_power_state = PWR_CTRL_STATE_ON;
        if (mock_power_state == PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF) mock_power_state = PWR_CTRL_STATE_OFF;
        step_power(20U);
        if (!pc_dispatch_next_response(89U, &response)) continue;
        CHECK(!reported);
        cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 89U);
        number_is(body, "requestCommandId", PC_COMMAND_SLT_START);
        string_is(body, "status", "failed"); string_is(body, "errorCode", error);
        cJSON_Delete(body); reported = 1;
    }
    CHECK(reported && pc_test_service_busy() == stays_busy && queue_calls == queued);
}

static void slt_preparation_check(void)
{
    power_fixture = 1; configure_power(); round_begin();
    unsigned on = power_on_calls, off = power_off_calls;
    handoff_test.begin_failure = 1;
    round_command(PC_COMMAND_SLT_START, SLT_START, "failed", "busy");
    CHECK(!pc_test_service_busy() && !pc_power_service_busy() && !handoff_test.reserved);
    CHECK(power_on_calls == on && power_off_calls == off);
    handoff_test.begin_failure = 0;
    handoff_test.prepared_failure = 1;
    round_command(PC_COMMAND_SLT_START, SLT_START, "accepted", NULL);
    slt_prepare_failure("notReady", 0);
    CHECK(handoff_test.cancelled && handoff_test.cancel_calls == 1U && handoff_mcu_busy());
    round_command(PC_COMMAND_START_TEST, COLD_START, "failed", "busy");
    round_end();

    /* 自动前级在普通时序准备前进入EDK：拒绝准备完成，不能把时序投递到串口网卡。 */
    configure_power(); round_begin();
    cJSON *start = cJSON_Parse(SLT_START); CHECK(start);
    CHECK(cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(start, "TimingParameters"), "tRCD", "35"));
    char *json = cJSON_PrintUnformatted(start); CHECK(json); cJSON_Delete(start);
    round_command(PC_COMMAND_SLT_START, json, "accepted", NULL); cJSON_free(json);
    handoff_test.cancel_allowed = 0;
    handoff_test_set_status(HANDOFF_SLT_EARLY_EDK);
    slt_prepare_failure("notReady", 1);
    CHECK(handoff_test.cancel_calls == 1U && !handoff_test.prepared && handoff_mcu_busy());
    slt_finish_session(); round_end();
    message("PASS: automatic SLT reservation/prepared failure and early EDK reject timing; cancellable and running ownership differ\n");
}

static void slt_lifecycle_check(void)
{
    pc_dispatch_response_t response;
    power_fixture = 1; configure_power(); round_begin();
    /* 一个0106内先完成FUNC，再进入SLT；新的预约不得覆盖前项结果或默认配置。 */
    round_command(PC_COMMAND_START_TEST, COLD_START, "accepted", NULL);
    cold_to_ready(); ready_starts_algorithm();
    slt_legacy_rejected();
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(90U, &response) == 1 && !pc_test_service_busy());
    cJSON *body = decode(&response, PC_COMMAND_TEST_RESULT_REPORT, 90U); cJSON_Delete(body);
    cJSON *start = cJSON_Parse(SLT_START); CHECK(start);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(start, "bootMode", cJSON_CreateString("fspiM1")));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(start, "power"),
        "vdd1", cJSON_CreateString("1810")));
    CHECK(cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(start, "TimingParameters"), "Freq", "2133"));
    char *json = cJSON_PrintUnformatted(start); CHECK(json); cJSON_Delete(start);
    round_command(PC_COMMAND_SLT_START, json, "accepted", NULL); cJSON_free(json);
    CHECK(handoff_test.reserved && handoff_test.cookie && !handoff_test.prepared);
    slt_complete(0);
    CHECK(handoff_test.prepared && handoff_test.prepared_calls == 1U && last_boot_mode == BOOT_MODE_FSPI_M1);
    uint32_t frequency_token;
    CHECK(rk_link_frequency_get(&frequency_token) == 2133 && frequency_token == handoff_test.cookie);
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDD1) == 1810U);
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDD2) == 1100U);
    CHECK(ddr_power_get_training_mv(DDR_RAIL_VDDQ) == 600U);
    /* 旧010B不能提前关电；RELEASE最终OK未TC期间持续占用。 */
    unsigned off = power_off_calls;
    slt_legacy_rejected();
    slt_frequency_is(frequency_token, 0);
    for (unsigned i = 0U; i < 3U; ++i) {
        step_power(100U);
        CHECK(pc_dispatch_next_response(91U, &response) == 0 && pc_test_service_busy());
        CHECK(power_off_calls == off && mock_power_state == PWR_CTRL_STATE_ON);
    }
    slt_frequency_is(frequency_token, 0);
    rk_link_frequency_note_tx(frequency_token, 1);
    slt_frequency_is(frequency_token, 1);
    round_command(PC_COMMAND_START_TEST, COLD_START, "failed", "busy");
    round_command(PC_COMMAND_POWER_OFF, "{}", "failed", "busy");
    CHECK(dispatch(PC_COMMAND_CLIENT_HEARTBEAT, "{}", 91U, &response) == 1);
    body = decode(&response, PC_COMMAND_SERVER_HEARTBEAT, 91U); cJSON_Delete(body);
    CHECK(dispatch(PC_COMMAND_QUERY_POWER_MONITOR, "{}", 91U, &response) == 1);
    handoff_test_set_status(HANDOFF_SLT_RELEASED); slt_finish_off();
    /* SLT后仍在同一轮；PARA按PC自己的电源/Boot配置重新准备并正常报告0109。 */
    start = cJSON_Parse(COLD_START); CHECK(start);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(start, "algorithmType", cJSON_CreateString("parametric")));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(start, "algorithmName", cJSON_CreateString("idd_0")));
    json = cJSON_PrintUnformatted(start); CHECK(json); cJSON_Delete(start);
    round_command(PC_COMMAND_PARA_START, json, "accepted", NULL); cJSON_free(json);
    cold_to_ready(); confirm_power(1); step_power(0U);
    CHECK(pc_dispatch_next_response(92U, &response) == 1);
    body = decode(&response, PC_COMMAND_COMMAND_RESP, 92U);
    number_is(body, "requestCommandId", PC_COMMAND_PARA_START); string_is(body, "status", "completed");
    cJSON_Delete(body);
    CHECK(queued_request.command_id == 21U && last_boot_mode == BOOT_MODE_EMMC);
    slt_legacy_rejected();
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(93U, &response) == 1 && !pc_test_service_busy());
    body = decode(&response, PC_COMMAND_PARA_RESULT, 93U); cJSON_Delete(body);
    round_end();

    /* 没有任何010B，EDK归还后仍须自动OFF并释放占用。 */
    configure_power(); round_begin();
    round_command(PC_COMMAND_SLT_START, SLT_START, "accepted", NULL); slt_complete(0);
    slt_finish_session();
    slt_legacy_rejected(); round_end();

    /* EDK仍占用协议口时收到0107也要关电；010B前后到达均不影响收尾。 */
    for (unsigned report_first = 0U; report_first < 2U; ++report_first) {
        configure_power(); round_begin();
        frequency_token = slt_frequency_start(); slt_complete(0);
        handoff_test.cancel_allowed = 0;
        if (report_first) slt_legacy_rejected();
        off = power_off_calls;
        round_end(); unsigned writes = led_writes;
        CHECK(mock_power_state == PWR_CTRL_STATE_OFF && power_off_calls == off + 1U);
        CHECK(!pc_test_service_busy() && !handoff_mcu_busy());
        CHECK(rk_link_frequency_get(NULL) == -1 && rk_link_frequency_status(frequency_token) == -1);
        if (!report_first) slt_legacy_rejected();
        CHECK(pc_dispatch_next_response(96U, &response) == 0);
        CHECK(led_writes == writes);
        slt_legacy_rejected();
        round_command(PC_COMMAND_START_TEST, COLD_START, "failed", "invalidState");
    }

    /* 从未发010B也允许断线后自动收尾；新连接不接收旧响应，归还前仍不能开新轮。 */
    configure_power(); round_begin();
    frequency_token = slt_frequency_start(); slt_complete(0);
    pc_dispatch_disconnected();
    slt_frequency_is(frequency_token, 0);
    slt_legacy_rejected();
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "failed", "busy");
    off = power_off_calls;
    step_power(100U); CHECK(power_off_calls == off && pc_test_service_busy());
    rk_link_frequency_note_tx(frequency_token, 1);
    slt_frequency_is(frequency_token, 1);
    slt_finish_session();
    round_begin(); slt_legacy_rejected(); round_end();
    message("PASS: automatic SLT release without 010B, early 0107 shuts down, disconnect preserves running SLT\n");
}

typedef enum { ROUND_CASE_FUNC, ROUND_CASE_PARA, ROUND_CASE_SLT } round_case_t;

/* 每项独立使用PC的电源/Boot参数；这里显式reboot，避免排列测试依赖上一项ready缓存。 */
static void round_run_item(round_case_t kind, uint32_t index)
{
    if (kind == ROUND_CASE_SLT) {
        unsigned begun = handoff_test.begin_calls;
        (void)slt_frequency_start(); slt_complete(0); slt_finish_session();
        CHECK(handoff_test.begin_calls == begun + 1U && !handoff_mcu_busy());
        CHECK(mock_power_state == PWR_CTRL_STATE_OFF);
        return;
    }
    const char *type = kind == ROUND_CASE_FUNC ? "functional" : "parametric";
    const char *name = kind == ROUND_CASE_FUNC ? "gtest-mul" : "idd_2P";
    uint16_t command = kind == ROUND_CASE_FUNC ? PC_COMMAND_START_TEST : PC_COMMAND_PARA_START;
    uint16_t result_command = kind == ROUND_CASE_FUNC ? PC_COMMAND_TEST_RESULT_REPORT : PC_COMMAND_PARA_RESULT;
    cJSON *start = cJSON_Parse(COLD_START); CHECK(start);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(start, "algorithmType", cJSON_CreateString(type)));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(start, "algorithmName", cJSON_CreateString(name)));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(start, "algorithmIndex", cJSON_CreateNumber(index)));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(start, "reboot", cJSON_CreateBool(1)));
    char *json = cJSON_PrintUnformatted(start); CHECK(json); cJSON_Delete(start);
    unsigned queued = queue_calls;
    round_command(command, json, "accepted", NULL); cJSON_free(json);
    if (pc_power_service_diagnostic()->phase == PC_POWER_REQUEST_OFF) direct_off_to_hold();
    cold_to_ready(); confirm_power(1); step_power(0U);
    pc_dispatch_response_t response;
    CHECK(pc_dispatch_next_response(98U, &response) == PC_DISPATCH_RESPONSE_READY);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 98U);
    number_is(body, "requestCommandId", command); number_is(body, "algorithmIndex", index);
    string_is(body, "status", "completed"); cJSON_Delete(body);
    CHECK(queue_calls == queued + 1U);
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(99U, &response) == PC_DISPATCH_RESPONSE_READY);
    body = decode(&response, result_command, 99U);
    string_is(body, "algorithmName", name); number_is(body, "algorithmIndex", index); cJSON_Delete(body);
    CHECK(!pc_test_service_busy() && !pc_power_service_busy());
}

static void slt_order_check(void)
{
    static const round_case_t orders[][3] = {
        {ROUND_CASE_FUNC, ROUND_CASE_PARA, ROUND_CASE_SLT},
        {ROUND_CASE_FUNC, ROUND_CASE_SLT, ROUND_CASE_PARA},
        {ROUND_CASE_PARA, ROUND_CASE_FUNC, ROUND_CASE_SLT},
        {ROUND_CASE_PARA, ROUND_CASE_SLT, ROUND_CASE_FUNC},
        {ROUND_CASE_SLT, ROUND_CASE_FUNC, ROUND_CASE_PARA},
        {ROUND_CASE_SLT, ROUND_CASE_PARA, ROUND_CASE_FUNC}
    };
    static const uint32_t indices[] = {37U, 1U, 37U};
    for (unsigned order = 0U; order < sizeof(orders) / sizeof(orders[0]); ++order) {
        configure_power(); round_begin();
        for (unsigned item = 0U; item < sizeof(indices) / sizeof(indices[0]); ++item)
            round_run_item(orders[order][item], indices[item]);
        round_end();
    }
    /* 重复SLT必须重新begin并完成一次独立OFF；同名FUNC/PARA和相同index不被当成重放。 */
    configure_power(); round_begin();
    round_run_item(ROUND_CASE_SLT, 0U);
    uint32_t first_cookie = handoff_test.cookie;
    unsigned off = power_off_calls;
    round_run_item(ROUND_CASE_SLT, 0U);
    CHECK(handoff_test.cookie != first_cookie && power_off_calls == off + 1U);
    round_run_item(ROUND_CASE_FUNC, 37U);
    round_run_item(ROUND_CASE_FUNC, 37U);
    round_run_item(ROUND_CASE_PARA, 1U);
    round_run_item(ROUND_CASE_PARA, 1U);
    round_end();
    message("PASS: all six FUNC/PARA/SLT orders in one 0106, repeated SLT/FUNC/PARA, non-monotonic/repeated algorithmIndex\n");
}

/* 故障状态按正式合同持续占用，故独立进程运行，不能添加测试专用强制清状态。 */
#if defined(PC_SLT_FAULT_TEST)
static void slt_fault_check(void)
{
    pc_dispatch_response_t response;
    power_fixture = 1; configure_power(); round_begin();
    round_command(PC_COMMAND_SLT_START, SLT_START, "accepted", NULL); slt_complete(0);
    unsigned off = power_off_calls;
    const char *error = "executionFailed";
#if PC_SLT_FAULT_TEST == 1
    slt_legacy_rejected();
    handoff_test_set_status(HANDOFF_SLT_RELEASED);
    CHECK(pc_dispatch_next_response(91U, &response) == 0);
    power_failure = PWR_CTRL_ERR_FAULT; step_power(1U);
    CHECK(power_off_calls == off + 1U);
#elif PC_SLT_FAULT_TEST == 2
    handoff_test_set_status(HANDOFF_SLT_FAILED);
#elif PC_SLT_FAULT_TEST == 3
    slt_legacy_rejected();
    handoff_test_set_status(HANDOFF_SLT_FAILED);
#else
    mock_power_state = PWR_CTRL_STATE_OFF;
    handoff_test_set_status(HANDOFF_SLT_OFF_READY);
    error = "powerOff";
#endif
    CHECK(pc_dispatch_next_response(92U, &response) == PC_DISPATCH_RESPONSE_READY);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 92U);
    number_is(body, "requestCommandId", PC_COMMAND_SLT_START);
    string_is(body, "status", "failed"); string_is(body, "errorCode", error); cJSON_Delete(body);
    CHECK(pc_test_service_busy());
    off = power_off_calls;
    for (unsigned i = 0U; i < 4U; ++i) {
        step_power(1000U);
        CHECK(pc_dispatch_next_response(93U, &response) == 0 && pc_test_service_busy());
        CHECK(power_off_calls == off);
    }
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "failed", "busy");
    round_command(PC_COMMAND_START_TEST, COLD_START, "failed", "busy");
#if PC_SLT_FAULT_TEST == 1
    slt_legacy_rejected();
#endif
    pc_dispatch_disconnected();
    slt_legacy_rejected();
    CHECK(pc_dispatch_next_response(94U, &response) == 0 && pc_test_service_busy());
    message("PASS: PC RK service SLT injected fault is reported once; busy retained, no automatic OFF retry or delivery to new connection\n");
}
#endif
#endif

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
    mutated_slt(PC_COMMAND_SLT_START, SLT_START, "power", cJSON_CreateNull(), "invalidArgument");
    mutated_slt(PC_COMMAND_SLT_START, SLT_START, "bootMode", cJSON_CreateString(""), "invalidArgument");
    mutated_slt(PC_COMMAND_SLT_START, SLT_START, "reboot", cJSON_CreateString("true"), "invalidArgument");
    CHECK(!pc_test_service_busy());
    round_command(PC_COMMAND_SLT_RESULT, SLT_RESULT, "failed", SLT_LEGACY_STATE_ERROR);
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
    round_command(PC_COMMAND_SLT_RESULT, SLT_RESULT, "failed", SLT_LEGACY_STATE_ERROR);
    slt_complete(0);
#if defined(PC_POWER_SEQUENCE_TEST)
    CHECK(rk_arms == arms);
#endif
    /* 尚无结束证据不能释放SLT；B1等EDK归还，V2保持旧PC报告流程。 */
    mock_tick += 3600000U; pc_dispatch_poll();
    CHECK(pc_dispatch_next_response(82U, &response) == 0 && pc_test_service_busy());
    round_command(PC_COMMAND_START_TEST, START_JSON, "failed", "busy");
    round_command(PC_COMMAND_SLT_START, SLT_START, "failed", "busy");
    round_command(PC_COMMAND_POWER_OFF, "{}", "failed", "busy");
    CHECK(dispatch(PC_COMMAND_QUERY_POWER_MONITOR, "{}", 82U, &response) == 1);
    mutated_slt(PC_COMMAND_SLT_RESULT, SLT_RESULT, "algorithmName", cJSON_CreateString("old"), SLT_LEGACY_STATE_ERROR);
    mutated_slt(PC_COMMAND_SLT_RESULT, SLT_RESULT, "siteId", cJSON_CreateString("SITE-8"), SLT_LEGACY_STATE_ERROR);
    mutated_slt(PC_COMMAND_SLT_RESULT, SLT_RESULT, "algorithmIndex", cJSON_CreateNumber(1), SLT_LEGACY_ARGUMENT_ERROR);
    mutated_slt(PC_COMMAND_SLT_RESULT, SLT_RESULT, "measurementData", cJSON_CreateObject(), SLT_LEGACY_ARGUMENT_ERROR);
    mutated_slt(PC_COMMAND_SLT_RESULT, SLT_RESULT, "testTimeMs", cJSON_CreateNumber(-1), SLT_LEGACY_ARGUMENT_ERROR);
    mutated_slt(PC_COMMAND_SLT_RESULT, SLT_RESULT, "testResults", cJSON_Parse("[{\"mirrorIndex\":0,\"testStatus\":0,\"resultDesc\":null,\"physicalAddr\":\"\"},{\"mirrorIndex\":0,\"testStatus\":1,\"resultDesc\":null,\"physicalAddr\":\"\"}]"), SLT_LEGACY_ARGUMENT_ERROR);
    CHECK(pc_test_service_busy());
    slt_finish_session();
    CHECK(!pc_test_service_busy());
    round_command(PC_COMMAND_SLT_RESULT, SLT_RESULT, "failed", SLT_LEGACY_STATE_ERROR);
    /* 每次新SLT都须重新启动；true/false均只执行一次，忙时重复请求不得再启动。 */
    for (unsigned reboot = 0U; reboot < 2U; ++reboot) {
#if defined(PC_POWER_SEQUENCE_TEST)
        unsigned previous_on = power_on_calls, previous_off = power_off_calls, previous_boot = boot_calls;
        unsigned expected_off = mock_power_state == PWR_CTRL_STATE_ON ? 1U : 0U;
#else
        uint32_t previous_cycles = v2_power_sim_cycles();
#endif
        cJSON *start = cJSON_Parse(SLT_START); CHECK(start);
        CHECK(cJSON_ReplaceItemInObjectCaseSensitive(start, "reboot", cJSON_CreateBool(reboot)));
        if (reboot) CHECK(cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(start, "TimingParameters"), "Freq", "2133"));
        char *json = cJSON_PrintUnformatted(start); CHECK(json);
        round_command(PC_COMMAND_SLT_START, json, "accepted", NULL);
        CHECK(cJSON_IsObject(pc_test_service_timing_parameters()) &&
              cJSON_GetArraySize(pc_test_service_timing_parameters()) == (int)reboot);
        round_command(PC_COMMAND_SLT_START, json, "failed", "busy");
        cJSON_free(json); cJSON_Delete(start);
        slt_complete(0);
        /* 仅填Freq时可先完成电源准备；DDR随后查询仍取本次值，实际收尾后恢复默认。 */
        if (reboot) {
            uint32_t token;
            CHECK(rk_link_frequency_get(&token) == 2133 && token && rk_link_frequency_status(token) == 0);
            rk_link_frequency_note_tx(token, 1);
            CHECK(pc_dispatch_next_response(81U, &response) == 0);
        }
#if defined(PC_POWER_SEQUENCE_TEST)
        CHECK(power_on_calls == previous_on + 1U && power_off_calls == previous_off + expected_off);
        CHECK(boot_calls == previous_boot + 1U && rk_arms == arms);
#else
        CHECK(v2_power_sim_cycles() == previous_cycles + 1U);
#endif
        slt_finish_session();
        CHECK(!pc_test_service_busy() && rk_link_frequency_get(NULL) == -1);
    }
    /* SLT上电准备阶段收到0107。B1撤销预约并维持OFF，V2保持原来的完成路径。 */
#if defined(PC_POWER_SEQUENCE_TEST)
    unsigned on = power_on_calls, off = power_off_calls, boot = boot_calls;
    unsigned expected_off = mock_power_state == PWR_CTRL_STATE_ON ? 1U : 0U;
#else
    uint32_t cycles = v2_power_sim_cycles();
#endif
#if defined(TEST_B1_AUTO_SLT)
    uint32_t discarded_frequency_token = slt_frequency_start();
#else
    round_command(PC_COMMAND_SLT_START, SLT_START, "accepted", NULL);
#endif
    round_end(); unsigned writes = led_writes;
#if defined(TEST_B1_AUTO_SLT)
    CHECK(rk_link_frequency_get(NULL) == -1 && rk_link_frequency_status(discarded_frequency_token) == -1);
    CHECK(mock_power_state == PWR_CTRL_STATE_OFF && !handoff_mcu_busy());
    CHECK(power_on_calls == on && power_off_calls == off + expected_off && boot_calls == boot);
    CHECK(pc_dispatch_next_response(82U, &response) == 0);
#else
    slt_complete(1);
#if defined(PC_POWER_SEQUENCE_TEST)
    CHECK(power_on_calls == on + 1U && power_off_calls == off + expected_off && boot_calls == boot + 1U);
#else
    CHECK(v2_power_sim_cycles() == cycles + 1U);
#endif
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "failed", "busy");
    slt_finish_session();
#endif
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
#if defined(TEST_B1_AUTO_SLT)
    CHECK(mock_power_state == PWR_CTRL_STATE_OFF);
#else
    cold_to_ready(); confirm_power(1); step_power(0U);
#endif
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
#if defined(TEST_B1_AUTO_SLT)
    uint32_t abandoned_frequency_token = slt_frequency_start(); slt_complete(0);
#else
    round_command(PC_COMMAND_SLT_START, SLT_START, "accepted", NULL); slt_complete(0);
#endif
    /* 断线不认领旧SLT，无实例ID时不能用新连接的同名报告冒充本次结果。 */
    pc_dispatch_disconnected();
#if defined(TEST_B1_AUTO_SLT)
    slt_frequency_is(abandoned_frequency_token, 0);
    rk_link_frequency_note_tx(abandoned_frequency_token, 1);
    slt_frequency_is(abandoned_frequency_token, 1);
#endif
    round_command(PC_COMMAND_SLT_RESULT, SLT_RESULT, "failed", SLT_LEGACY_STATE_ERROR);
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "failed", "busy");
    CHECK(pc_test_service_busy());
#if defined(TEST_B1_AUTO_SLT)
    slt_finish_session();
    slt_legacy_rejected();
    round_begin(); round_end();
#endif
    message("PASS: PC RK service SLT always cold-starts even with same target/reboot=false; busy does not restart; no RK ready; target-specific completion and ownership\n");
}
#endif
#endif

#if defined(TEST_B1_PROTOCOL_TRACE)
/* 模拟TCP已经收齐一个PDU，校验业务及组帧实际发布给日志的元数据。 */
static void trace_exchange(uint16_t command, const char *json, uint32_t sequence)
{
    pc_protocol_frame_t frame = {.command_id = command, .sequence = sequence,
        .json = (const uint8_t *)json, .json_length = (uint32_t)strlen(json)};
    frame.length = PC_PROTOCOL_HEADER_SIZE + frame.json_length;
    pc_dispatch_response_t reply;
    pc_trace_rx(&frame);
    int result = pc_dispatch_handle(&frame, sequence, &reply);
    if (command == PC_COMMAND_ROUND_END && result == PC_DISPATCH_NO_RESPONSE) {
        complete_test_power_off();
        result = pc_dispatch_next_response(sequence, &reply);
    }
    CHECK(result == PC_DISPATCH_RESPONSE_READY);
    pc_trace_tx(&reply, sequence, reply.length, 0);
}

static void pc_protocol_trace_check(void)
{
    static const char start[] = "{\"algorithmType\":\"functional\",\"algorithmName\":\"gtest-mul\","
        "\"algorithmIndex\":1,\"power\":null,\"TimingParameters\":{},\"reboot\":false,\"bootMode\":null}";
    pc_dispatch_response_t result;
    pc_dispatch_disconnected();
    mock_power_state = PWR_CTRL_STATE_ON;
    const uint8_t peer_ip[] = {192U, 168U, 1U, 111U};
    trace_capture_begin(); pc_trace_connected(peer_ip, 59178U);
    CHECK(strstr(trace_capture_text(), "connected peer=192.168.1.111:59178 listener=closed"));
    trace_exchange(PC_COMMAND_ROUND_BEGIN, "{}", 1U);
#if defined(PC_COMMAND_CHIP_INFO)
    observe_ddrbin_boot(1024U);
    CHECK(pc_dispatch_next_response(60U, &result) == PC_DISPATCH_RESPONSE_READY);
    CHECK(result.command_id == PC_COMMAND_CHIP_INFO);
    pc_trace_tx(&result, 60U, result.length, 0);
    CHECK(strstr(trace_capture_text(),
        "{\"chipinfo\":[{\"mirrorIndex\":0,\"MemoryType\":\"LPDDR4X\",\"MemorySize\":2048},"
        "{\"mirrorIndex\":1,\"MemoryType\":\"LPDDR4X\",\"MemorySize\":2048}]}\r\n"));
    pc_dispatch_response_sent(PC_COMMAND_CHIP_INFO);
#endif
    trace_exchange(PC_COMMAND_START_TEST, start, 2U);
    CHECK(strstr(trace_capture_text(), "[PC RX] conn=1 cmd=0106 seq=1 len=20\r\n"));
    CHECK(strstr(trace_capture_text(), "req=0106") && strstr(trace_capture_text(), "\"completed\""));
    CHECK(strstr(trace_capture_text(), "cmd=0103 type=functional test=gtest-mul idx=1"));
    CHECK(strstr(trace_capture_text(), "boot=external Freq=-1 timingCount=0"));
    CHECK(strstr(trace_capture_text(), "req=0103") && strstr(trace_capture_text(), "\"accepted\""));
    trace_capture_begin();
    for (unsigned i = 0; i < 10U; ++i) {
        trace_exchange(PC_COMMAND_CLIENT_HEARTBEAT, "{}", 3U);
        trace_exchange(PC_COMMAND_QUERY_POWER_MONITOR, "{}", 4U);
    }
    trace_exchange(PC_COMMAND_QUERY_POWER_MONITOR, "{", 5U);
    CHECK(trace_capture_text()[0] == '\0');
    trace_report(trace_capture_append);
    CHECK(strstr(trace_capture_text(), "PC heartbeat rx=10 tx=10 errors=0"));
    CHECK(strstr(trace_capture_text(), "PC sample rx=11 tx=10 errors=1"));
    trace_capture_begin();
    inject(RK_RESPONSE_COMPLETE, UINT64_C(0x200038));
    queued_response.result.elapsed_ms = 5497U;
    pc_json_configure_allocator(no_memory, free);
    CHECK(pc_dispatch_next_response(6U, &result) == 0);
    pc_json_configure_allocator(NULL, NULL);
    CHECK(pc_dispatch_next_response(6U, &result) == PC_DISPATCH_RESPONSE_READY);
    const char *first = strstr(trace_capture_text(),
        "[RESULT] gtest-mul idx=1 time=5497ms die0=FAIL@0x0000000000200038 die1=PASS");
    CHECK(first && !strstr(first + 1, "[RESULT]"));
    CHECK(result.command_id == PC_COMMAND_TEST_RESULT_REPORT && result.result_index == 1U);
    pc_trace_tx(&result, 6U, 24U, 104);
    CHECK(strstr(trace_capture_text(), "cmd=0105 idx=1 seq=6 len="));
    CHECK(strstr(trace_capture_text(), "tcpQueued=24 sendFailed errno=104"));
    trace_capture_begin(); pc_trace_tx(&result, 6U, result.length, 0);
    char bytes[48];
    snprintf(bytes, sizeof(bytes), "len=%lu tcpQueued=%lu\r\n", (unsigned long)result.length, (unsigned long)result.length);
    CHECK(strstr(trace_capture_text(), bytes));
    trace_exchange(PC_COMMAND_START_TEST, start, 7U);
    trace_exchange(PC_COMMAND_ROUND_END, "{\"failChips\":[]}", 8U);
    CHECK(strstr(trace_capture_text(), "discard pending result reason=0107"));
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(9U, &result) == 0);
    pc_trace_closed("peerClosed"); pc_dispatch_disconnected();
    CHECK(strstr(trace_capture_text(), "conn=1 closed reason=peerClosed peer=192.168.1.111:59178"));
    trace_capture_stop();
    message("PASS: B1 PC protocol log, polling counters, result retry, full/partial TX and early 0107\n");
}
#endif

/* 同时核对结果及0602统计的实际JSON文本，避免只看double值漏掉长小数输出。 */
static void measurement_precision_check(void)
{
    static const struct {
        int64_t sum_ua;
        uint32_t count;
        const char *json_ma;
    } cases[] = {
        {6712, 3U, "2.237"},
        {9005, 3U, "3.002"},
        {1, 2U, "0.001"},
        {1, 3U, "0"},
        {19999, 2U, "10"},
        {(int64_t)UINT32_MAX * INT32_MAX, INT32_MAX, "4294967.295"},
        {0, 0U, "null"}
    };
    for (unsigned i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        idd_stats_t snapshot = {.state = IDD_STATS_COMPLETE};
        snapshot.voltage[0] = (idd_metric_stats_t){.mean = 1800, .valid_count = 1U};
        snapshot.current[0] = (idd_metric_stats_t){.sum = cases[i].sum_ua,
            .minimum = 0, .maximum = UINT32_MAX, .valid_count = cases[i].count};
        cJSON *body = cJSON_CreateObject(); CHECK(body);
        CHECK(cJSON_AddNullToObject(body, "measurementData"));
        CHECK(pc_test_add_measurement(body, &snapshot));
        CHECK(pc_test_add_statistics(body, &snapshot));
        const cJSON *measurement = cJSON_GetObjectItemCaseSensitive(body, "measurementData");
        const cJSON *statistics = cJSON_GetObjectItemCaseSensitive(body, "statistics");
        const cJSON *values[] = {
            cJSON_GetObjectItemCaseSensitive(measurement, "Idd1Ave"),
            cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(statistics, "currentAvgMa"), 0)
        };
        for (unsigned j = 0U; j < sizeof(values) / sizeof(values[0]); ++j) {
            CHECK(cases[i].count ? cJSON_IsNumber(values[j]) : cJSON_IsNull(values[j]));
            char *json = cJSON_PrintUnformatted(values[j]); CHECK(json);
            CHECK(strcmp(json, cases[i].json_ma) == 0);
            cJSON_free(json);
        }
        CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(measurement, "Idd2Ave")));
        number_is(measurement, "Vdd1Ave", 1800U);
        cJSON_Delete(body);
    }
    message("PASS: PC RK service measurement JSON rounding, 3 decimals, query/result consistency and null\n");
}

#if defined(PC_COMMAND_CHIP_INFO) && defined(PC_POWER_SEQUENCE_TEST) && defined(PC_TEST_ROUND_TEST)
static void feed_ddr_log(const char *text)
{
    rk_ddr_capacity_observe(true, (const uint8_t *)text, (int)strlen(text));
}

static void chip_info_first_tests(void)
{
    pc_dispatch_response_t response;
    power_fixture = 1;
    configure_power();
    /* 旧启动的完整缓存不能单独触发下一轮的010D。 */
    observe_ddrbin_boot(4096U);
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "completed", NULL);
    CHECK(pc_dispatch_next_response(70U, &response) == 0);
    /* 旧启动已读全但尚未上报；本次冷启动只能采用下次上电的通道表。 */
    observe_ddrbin_boot(4096U);
    round_command(PC_COMMAND_START_TEST, COLD_START, "accepted", NULL);
    unsigned queued = queue_calls;
    CHECK(pc_dispatch_next_response(70U, &response) == 0);
    rk_ddr_capacity_observe(false, NULL, 0);
    /* PC 已受理很久，但 RK 尚未上电，不能在 OFF 停留阶段冻结空容量。 */
    mock_tick += B1_RK_CHIP_INFO_TIMEOUT_MS;
    CHECK(pc_dispatch_next_response(70U, &response) == 0 && queue_calls == queued);
    step_power(0U); step_power(5U);
    mock_power_state = PWR_CTRL_STATE_ON; step_power(1U);
    feed_ddr_log("LPDDR4, 1600MHz\nchannel[3] Size=256MB\nchannel[1] Size=1024MB\n");
    confirm_power(1); step_power(0U);
    CHECK(pc_dispatch_next_response(70U, &response) == 0 && queue_calls == queued);
    feed_ddr_log("channel[0] Size=512MB\nchannel[2] Size=2048MB\n");
    pc_json_configure_allocator(no_memory, free);
    CHECK(pc_dispatch_next_response(70U, &response) == 0 && queue_calls == queued);
    pc_json_configure_allocator(NULL, NULL);
    check_chip_report(70U, "LPDDR4", 1536U, "LPDDR4", 2304U);
    pc_dispatch_response_sent(PC_COMMAND_SERVER_HEARTBEAT);
    CHECK(pc_dispatch_next_response(71U, &response) == 0 && queue_calls == queued);
    pc_dispatch_response_sent(PC_COMMAND_CHIP_INFO);
    CHECK(pc_dispatch_next_response(71U, &response) == 1 && queue_calls == queued + 1U);
    cJSON *body = decode(&response, PC_COMMAND_COMMAND_RESP, 71U);
    string_is(body, "status", "completed"); cJSON_Delete(body);
    result_is(0U, NULL, site_label);
    round_command(PC_COMMAND_START_TEST, START_JSON, "accepted", NULL);
    CHECK(queue_calls == queued + 2U && pc_dispatch_next_response(72U, &response) == 0);
    result_is(0U, NULL, site_label);
    round_end();

    /* 外部已启动的PARA沿用同次日志；没有表尾时跨tick回绕仍按期限上报三路。 */
#if defined(PC_COMMAND_CHIP_INFO)
    mock_power_state = PWR_CTRL_STATE_ON;
#endif
    rk_ddr_capacity_observe(false, NULL, 0);
    feed_ddr_log("LPDDR4X, 2133MHz\nchannel[0] Size=256MB\nchannel[1] Size=512MB\n"
                 "channel[2] Size=2048MB\n");
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "completed", NULL);
    mock_tick = UINT32_MAX - B1_RK_CHIP_INFO_TIMEOUT_MS / 2U;
    char *para = para_json("parametric", "idd_2NS", 3U);
    round_command(PC_COMMAND_PARA_START, para, "accepted", NULL);
    cJSON_free(para);
    queued = queue_calls;
    mock_tick += B1_RK_CHIP_INFO_TIMEOUT_MS - 1U;
    CHECK(pc_dispatch_next_response(73U, &response) == 0 && queue_calls == queued);
    ++mock_tick;
    check_chip_report(73U, "LPDDR4X", 768U, "LPDDR4X", 2048U);
    observe_ddrbin_boot(2048U);
    rk_ddr_capacity_t frozen = rk_ddr_capacity_snapshot();
    CHECK(frozen.chips[0].size_mib == 768U && frozen.chips[1].size_mib == 2048U && frozen.current_mib == 2816U);
    pc_dispatch_response_sent(PC_COMMAND_CHIP_INFO);
    CHECK(pc_dispatch_next_response(74U, &response) == 0 && queue_calls == queued + 1U);
    CHECK(queued_request.command_id == 25U);
    inject(RK_RESPONSE_COMPLETE, 0U);
    CHECK(pc_dispatch_next_response(74U, &response) == 1 && response.command_id == PC_COMMAND_PARA_RESULT);
    round_end();

    /* SLT电源准备可结束；未发完整010D时不可授权EDK。 */
    configure_power();
    rk_ddr_capacity_observe(false, NULL, 0);
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "completed", NULL);
    mock_tick += B1_RK_CHIP_INFO_TIMEOUT_MS;
    CHECK(pc_dispatch_next_response(75U, &response) == 0);
    round_command(PC_COMMAND_SLT_START, SLT_START, "accepted", NULL);
    step_power(10U); step_power(5U); mock_power_state = PWR_CTRL_STATE_ON; step_power(1U);
    CHECK(pc_dispatch_next_response(75U, &response) == 0 && !handoff_test.prepared);
    feed_ddr_log("LPDDR4X, 2133MHz\nchannel[1] Size=512MB\nchannel[3] Size=1024MB\n");
    CHECK(pc_dispatch_next_response(75U, &response) == 0);
    feed_ddr_log("Manufacturer ID:0xff\n");
    check_chip_report(75U, "LPDDR4X", 512U, "LPDDR4X", 1024U);
    CHECK(pc_dispatch_next_response(76U, &response) == 0 && !handoff_test.prepared);
    pc_dispatch_response_sent(PC_COMMAND_CHIP_INFO);
    CHECK(pc_dispatch_next_response(76U, &response) == 1 && handoff_test.prepared);
    slt_finish_session(); round_end();

    /* 没有日志也没有SPL心跳：先上传空容量，再给出两颗DDR训练失败的0105。 */
    configure_power();
    rk_ddr_capacity_observe(false, NULL, 0);
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "completed", NULL);
    round_command(PC_COMMAND_CHIP_INFO, "{}", "failed", "invalidDirection");
    round_command(PC_COMMAND_START_TEST, COLD_START, "accepted", NULL);
    queued = queue_calls;
    cold_to_ready();
    /* cold_to_ready 在本次上电后又推进了 1 ms。到期前 1 ms 仍等待日志。 */
    mock_tick += B1_RK_CHIP_INFO_TIMEOUT_MS - 2U;
    CHECK(pc_dispatch_next_response(77U, &response) == 0);
    ++mock_tick;
    check_chip_report(77U, "", 0U, "", 0U);
    pc_dispatch_response_sent(PC_COMMAND_CHIP_INFO);
    step_power(0U);
    training_failed_result(0, 0U);
    CHECK(queue_calls == queued);
    round_end();

    /* 首项在启动供电前失败：仍交付原错误，缺失的容量按受理时间有界上报。 */
    configure_power();
    rk_ddr_capacity_observe(false, NULL, 0);
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "completed", NULL);
    round_command(PC_COMMAND_START_TEST, COLD_START, "accepted", NULL);
    boot_failure = 1;
    step_power(sequence_config.off_hold_ms);
    preparation_failed("executionFailed");
    mock_tick += B1_RK_CHIP_INFO_TIMEOUT_MS - sequence_config.off_hold_ms;
    check_chip_report(77U, "", 0U, "", 0U);
    pc_dispatch_response_sent(PC_COMMAND_CHIP_INFO);
    round_end();

    /* 已组出的010D未发完就断开：原首项不执行，新连接不能领取原轮的事件。 */
    mock_power_state = PWR_CTRL_STATE_ON;
    observe_ddrbin_boot(2048U);
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "completed", NULL);
    round_command(PC_COMMAND_START_TEST, START_JSON, "accepted", NULL);
    check_chip_report(78U, "LPDDR4X", 4096U, "LPDDR4X", 4096U);
    pc_dispatch_disconnected(); pc_dispatch_poll();
    CHECK(queue_calls == queued && !pc_test_service_busy() && pc_dispatch_next_response(1U, &response) == 0);
    /* 缺类型且只收到CH2，out结束本次训练；缺记录的镜像0保持空串和0。 */
    rk_ddr_capacity_observe(false, NULL, 0);
    round_command(PC_COMMAND_ROUND_BEGIN, "{}", "completed", NULL);
    feed_ddr_log("channel[2] Size=512MB\nout\nchannel[3] Size=1024MB\n");
    round_command(PC_COMMAND_START_TEST, START_JSON, "accepted", NULL);
    check_chip_report(2U, "", 0U, "", 512U);
    round_end();
    pc_dispatch_response_sent(PC_COMMAND_CHIP_INFO);
    CHECK(pc_dispatch_next_response(3U, &response) == 0 && !pc_test_service_busy() && queue_calls == queued);

    /* 超出double精确整数范围也使用完整的无引号uint64，与实际板级容量上限独立。 */
    cJSON *array = cJSON_CreateArray(); CHECK(array);
    CHECK(pc_chip_info_append(array, 0U, NULL, UINT64_MAX));
    char *json = cJSON_PrintUnformatted(array); CHECK(json);
    CHECK(strcmp(json, "[{\"mirrorIndex\":0,\"MemoryType\":\"\",\"MemorySize\":18446744073709551615}]") == 0);
    cJSON_free(json); cJSON_Delete(array);
    message("PASS: PC RK service 010D channel mapping, timeout, send gate, FUNC/PARA/SLT and disconnect\n");
}
#endif

void test_main(void)
{
    measurement_precision_check();
#if defined(PC_SLT_FAULT_TEST)
    slt_fault_check();
    finish(0U);
#endif
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
#if defined(TEST_B1_AUTO_SLT)
    slt_preparation_check();
    slt_lifecycle_check();
    slt_order_check();
#endif
    slt_flow_check();
#endif
#if defined(PC_COMMAND_CHIP_INFO) && defined(PC_POWER_SEQUENCE_TEST)
    chip_info_first_tests();
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
    /* 经过实际RK解码器及0105组包：FAIL保留零地址，未列颗粒及PASS帧仍输出PASS/null。 */
    for (uint8_t mask = 0U; mask < 4U; ++mask) {
        for (uint8_t zero_addresses = 0U; zero_addresses < 4U; ++zero_addresses) {
            if ((zero_addresses & ~mask) != 0U) continue;
            start_expect(START_JSON, NULL);
            inject_mask(RK_RESPONSE_COMPLETE, mask,
                        (zero_addresses & 1U) ? 0U : 1U, (zero_addresses & 2U) ? 0U : UINT64_MAX);
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
                if (failed) string_is(desc, "firstErrorAddress", (zero_addresses & (1U << die)) ?
                                      "0x0000000000000000" : die ? "0xFFFFFFFFFFFFFFFF" : "0x0000000000000001");
                else CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(desc, "firstErrorAddress")));
                string_is(chip, "physicalAddr", "");
            }
            cJSON_Delete(root);
            CHECK(!pc_test_service_busy());
        }
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
#if defined(TEST_B1_PROTOCOL_TRACE)
    pc_protocol_trace_check();
#endif
    finish(0U);
}
