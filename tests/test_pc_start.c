/* 在QEMU执行正式0103解析、分发和组帧；硬件入口一旦被调用立即判失败。 */
#include "pc_dispatch.h"
#include "pc_test_message.h"
#include "pc_tester_info.h"
#include "pc_ota.h"
#include "monitor_task.h"
#include "result_led.h"
#include "ota_app.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

#define S1(x) #x
#define S(x) S1(x)
#define CHECK(c) do { if (!(c)) fail("FAIL " S(__LINE__) ": " #c "\n"); } while (0)
#define VALID_JSON "{\"algorithmType\":\"parametric\",\"algorithmName\":\"idd_0\",\"algorithmIndex\":1,\"power\":{\"memoryType\":\"LPDDR4X\",\"vdd1Steps\":0,\"vdd2Steps\":0,\"vddqSteps\":0},\"reboot\":true,\"bootMode\":\"emmc\"}"
static int confirmed = 1;
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

/* 0103仅允许OTA健康查询；任何电源、DAC、采样、Flash或其他业务调用均失败。 */
int ota_app_is_confirmed(void) { return confirmed; }
uint32_t HAL_GetTick(void) { if (query_monitor) return 1000U; fail("FAIL unexpected HAL_GetTick\n"); }
pwr_ctrl_state_t pwr_ctrl_get_state(void) { fail("FAIL unexpected power access\n"); }
pwr_ctrl_result_t monitor_request_power_on(uint32_t now) { (void)now; fail("FAIL power on\n"); }
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
        /* 正式PAC换算函数和板级系数均不替换，只模拟已经采集到的寄存器值。 */
        out->memory.idd[rail].current_ma = pac1934_current_ma(0x8000U, PAC1934_RSENSE_B1_UOHM);
        out->memory.idd[rail].power_mw = pac1934_power_mw(0x08000000U, PAC1934_RSENSE_B1_UOHM);
    }
    return 1;
}
HAL_StatusTypeDef result_led_set(result_led_target_t target, result_led_state_t state)
{ (void)target; (void)state; fail("FAIL LED write\n"); }
HAL_StatusTypeDef result_led_get_all(result_led_state_t states[3])
{ (void)states; fail("FAIL LED query\n"); }
int pc_tester_info_load(fru_info_t *fru) { (void)fru; fail("FAIL Flash access\n"); }
pc_dispatch_result_t pc_ota_handle(const pc_protocol_frame_t *request, uint32_t sequence,
                                  const cJSON *root, pc_dispatch_response_t *response)
{ (void)request; (void)sequence; (void)root; (void)response; fail("FAIL OTA command\n"); }
void ota_download_poll(void) { fail("FAIL OTA poll\n"); }
void ota_download_disconnected(void) { fail("FAIL OTA disconnect\n"); }

static cJSON *base_request(void)
{
    cJSON *root = NULL;
    CHECK(pc_json_parse_object((const uint8_t *)VALID_JSON, strlen(VALID_JSON), &root, NULL) == PC_JSON_OK);
    return root;
}

/* 使用真实分发入口和响应JSON核对错误语义、独立Seq及index回带。 */
static void check_request(cJSON *root, const char *error, int has_index, uint32_t index)
{
    char *json = cJSON_PrintUnformatted(root);
    CHECK(json != NULL);
    pc_protocol_frame_t request = {
        .command_id = PC_COMMAND_START_TEST, .service_id = 0U, .sequence = 99U,
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
    CHECK(cJSON_GetObjectItemCaseSensitive(body, "requestCommandId")->valuedouble == 259.0);
    const cJSON *echo = cJSON_GetObjectItemCaseSensitive(body, "algorithmIndex");
    CHECK(has_index ? cJSON_IsNumber(echo) && echo->valuedouble == (double)index : echo == NULL);
    CHECK(ddr_power_get_memory_type() == DDR_MEMORY_LPDDR4X);
    CHECK(ddr_power_get_mv(DDR_RAIL_VDD1) == 1800U && ddr_power_get_mv(DDR_RAIL_VDD2) == 1100U &&
          ddr_power_get_mv(DDR_RAIL_VDDQ) == 600U);
    cJSON_Delete(body);
    cJSON_free(json);
    cJSON_Delete(root);
}

static void replace(cJSON *root, const char *key, cJSON *value)
{
    CHECK(value && cJSON_ReplaceItemInObjectCaseSensitive(root, key, value));
}

void test_main(void)
{
    const char *const required[] = {"algorithmType", "algorithmName", "algorithmIndex", "power", "reboot", "bootMode"};
    const char *const power_keys[] = {"memoryType", "vdd1Steps", "vdd2Steps", "vddqSteps"};
    cJSON *root = base_request();
    pc_test_start_view_t view;
    CHECK(pc_test_start_parse(root, &view) == PC_TEST_JSON_OK);
    CHECK(view.target_mv[0] == 1800U && view.target_mv[1] == 1100U && view.target_mv[2] == 600U);
    CHECK(view.reboot == 1U && view.algorithm_index == 1U);
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
        CHECK(pc_test_start_parse(root, &view) == PC_TEST_JSON_OK && view.target_mv[2] == 1100U && view.reboot == 0U);
        check_request(root, "notReady", 1, i == 0U ? 0U : UINT32_MAX);
    }
    for (size_t i = 0U; i < 6U; i++) {
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
    const double invalid_steps[] = {-32769.0, 32768.0, 0.5, 78.0, -51.0};
    for (size_t i = 0U; i < 5U; i++) {
        root = base_request(); replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "vdd1Steps", cJSON_CreateNumber(invalid_steps[i]));
        check_request(root, "invalidArgument", 1, 1U);
    }
    root = base_request(); replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "vddqSteps", cJSON_CreateBool(1));
    check_request(root, "invalidArgument", 1, 1U);
    root = base_request(); replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "vddqSteps", cJSON_CreateNumber(54));
    check_request(root, "invalidArgument", 1, 1U);
    root = base_request(); replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "vdd2Steps", cJSON_CreateNumber(29));
    check_request(root, "invalidArgument", 1, 1U);
    root = base_request(); replace(cJSON_GetObjectItemCaseSensitive(root, "power"), "vdd1Steps", cJSON_CreateNumber(77));
    CHECK(pc_test_start_parse(root, &view) == PC_TEST_JSON_OK && view.target_mv[0] == 3340U);
    check_request(root, "notReady", 1, 1U); /* 可写但极端值也不执行，不能当成安全批准。 */

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
        number = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "power"), "vdd2Steps");
        number->valuedouble = i == 0U ? NAN : INFINITY;
        CHECK(pc_test_start_parse(root, &view) != PC_TEST_JSON_OK);
        cJSON_Delete(root);
    }
    confirmed = 0;
    check_request(base_request(), "otaNotReady", 1, 1U);
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
        CHECK(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(body, "currentMa"), rail)->valueint ==
              (PC_TEST_EXPECTED_RSENSE == 20000U ? 2500 : 5000));
        CHECK(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(body, "powerMw"), rail)->valueint ==
              (PC_TEST_EXPECTED_RSENSE == 20000U ? 80000 : 160000));
    }
    cJSON_Delete(body);
    message("PASS: actual START_TEST parser/dispatch, index, OTA and zero hardware side effects\n");
    message("PASS: actual PAC conversion with board shunt reaches 0602 JSON\n");
    finish(0U);
}
