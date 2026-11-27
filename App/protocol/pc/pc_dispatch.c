#include "pc_dispatch.h"
#include "handoff_mcu.h"

#include "pc_command.h"
#include "pc_test_message.h"
#include "pc_tester_info.h"
#include "monitor_task.h"
#include "pwr_ctrl.h"
#include "result_led.h"
#include "pc_test_service.h"
#include "pc_site_control.h"
#include "pc_power_service.h"
#if defined(B1_RK_UART)
#include "log_console.h"
#include "rk_task_control.h"
#include <stdio.h>
#endif
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
#include "v2_power_sim.h"
/* 仅替换本文件的电源/测量依赖；正式监测任务、真实RK及Flash不变。 */
#define pc_power_state       v2_power_sim_state
#define pc_power_on          v2_power_sim_on
#define pc_boot_set          v2_power_sim_set_boot
#define pc_boot_get          v2_power_sim_get_boot
#define pc_ddr_set           v2_power_sim_set_ddr
#define pc_sample_get        v2_power_sim_latest
#else
#define pc_power_state       pwr_ctrl_get_state
#define pc_power_on          monitor_request_power_on
#define pc_boot_set          monitor_set_boot_mode
#define pc_boot_get          monitor_get_boot_mode
#define pc_ddr_set           monitor_apply_ddr_config
#define pc_sample_get        monitor_get_latest
#endif
#if defined(ENABLE_RK_GATEWAY_TEST) && ENABLE_RK_GATEWAY_TEST
#include "rk_gateway_test.h"
#endif

#include <string.h>

/* 电源命令的终态只能交给发起连接；断线后继续收尾，但不能发给新客户端。 */
static uint16_t s_power_command;
static pc_power_action_t s_power_action;
static uint8_t s_power_owner_connected;
#if defined(B1_RK_UART)
static log_console_power_request_t s_console_power;
static int64_t s_next_manual_frequency = -1; /* 控制台暂存，只有受理start rk才消费。 */
#endif

/*
 * 每个命令处理函数只负责本命令的字段校验和本地响应。
 * request/root均为借用：root由分发入口统一释放，处理函数不得释放或保存它。
 * response由TCP任务提供；需要回复时填入完整PDU，不在这里调用socket或RK。
 */
typedef pc_dispatch_result_t (*pc_command_handler_t)(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response);

typedef struct {
    uint16_t command_id;           /* 查找键：PC协议的CommandID，不是RK命令号。 */
    pc_command_handler_t handler; /* 命中后调用的唯一处理函数。 */
} pc_command_entry_t;

/* 复用统一组帧入口，确保所有MCU发送帧固定ServiceID=0并使用本端Seq。 */
static pc_dispatch_result_t build_response(
    uint16_t response_command_id,
    uint32_t mcu_tx_sequence,
    const uint8_t *json,
    uint32_t json_length,
    pc_dispatch_response_t *response)
{
    response->command_id = response_command_id;
    return pc_protocol_build_response(response_command_id, mcu_tx_sequence,
                                      json, json_length,
                                      response->data, sizeof(response->data),
                                      &response->length) == 0
               ? PC_DISPATCH_RESPONSE_READY
               : PC_DISPATCH_ERROR;
}

/*
 * 已收齐且帧头有效的请求，JSON或业务参数错误统一使用0x0006返回，不能把
 * 可定位的坏帧升级为TCP断连。失败固定status=failed；同步控制完成统一返回
 * status=completed，避免各命令自行发明应答外形。
 */
static pc_dispatch_result_t build_command_failure_with_index(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const char *error_code,
    const uint32_t *algorithm_index,
    pc_dispatch_response_t *response)
{
    cJSON *body = NULL;
    char json[256] = {0};
    pc_dispatch_result_t result;

    if (!request || !error_code) return PC_DISPATCH_ERROR;
    response->request_command = request->command_id;
    body = cJSON_CreateObject();
    if (!body ||
        !cJSON_AddNumberToObject(body, "requestCommandId", request->command_id) ||
        !cJSON_AddStringToObject(body, "status", "failed") ||
        !cJSON_AddStringToObject(body, "errorCode", error_code) ||
        (algorithm_index && !cJSON_AddNumberToObject(body, "algorithmIndex", *algorithm_index)) ||
        ((request->command_id == PC_COMMAND_SLT_START || request->command_id == PC_COMMAND_SLT_RESULT) &&
         !cJSON_AddNullToObject(body, "algorithmIndex")) ||
        !cJSON_PrintPreallocated(body, json, sizeof(json), 0)) {
        cJSON_Delete(body);
        return PC_DISPATCH_ERROR;
    }

    result = build_response(PC_COMMAND_COMMAND_RESP, mcu_tx_sequence,
                            (const uint8_t *)json,
                            (uint32_t)strlen(json), response);
    cJSON_Delete(body);
    return result;
}

/* 非测试命令没有算法计划项编号，保留既有错误响应合同。 */
static pc_dispatch_result_t build_command_failure(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const char *error_code,
    pc_dispatch_response_t *response)
{
    return build_command_failure_with_index(request, mcu_tx_sequence,
                                            error_code, NULL, response);
}

static pc_dispatch_result_t build_command_status(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const char *status,
    pc_dispatch_response_t *response)
{
    cJSON *body = NULL;
    char json[192] = {0};
    pc_dispatch_result_t result;

    if (!request || !status) return PC_DISPATCH_ERROR;
    response->request_command = request->command_id;
    body = cJSON_CreateObject();
    if (!body ||
        !cJSON_AddNumberToObject(body, "requestCommandId", request->command_id) ||
        !cJSON_AddStringToObject(body, "status", status) ||
        !cJSON_PrintPreallocated(body, json, sizeof(json), 0)) {
        cJSON_Delete(body);
        return PC_DISPATCH_ERROR;
    }

    result = build_response(PC_COMMAND_COMMAND_RESP, mcu_tx_sequence,
                            (const uint8_t *)json,
                            (uint32_t)strlen(json), response);
    cJSON_Delete(body);
    return result;
}

/* 同步控制已完成；异步电源请求则使用accepted并由PC继续查询状态。 */
static pc_dispatch_result_t build_command_success(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    pc_dispatch_response_t *response)
{
    return build_command_status(request, mcu_tx_sequence,
                                "completed", response);
}

static pc_dispatch_result_t build_command_accepted(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    pc_dispatch_response_t *response)
{
    return build_command_status(request, mcu_tx_sequence,
                                "accepted", response);
}

/* 新控制命令使用严格字段集合，防止拼写错误或旧字段被悄悄忽略。 */
static int object_has_exact_fields(const cJSON *root,
                                   const char *const *names,
                                   size_t count)
{
    size_t actual = 0U;
    for (const cJSON *item = root ? root->child : NULL; item; item = item->next)
        actual++;
    if (actual != count) return 0;

    for (size_t i = 0U; i < count; i++) {
        if (cJSON_GetObjectItemCaseSensitive(root, names[i]) == NULL)
            return 0;
    }
    return 1;
}

typedef struct {
    const char *name;
    uint8_t value;
} named_value_t;

static int read_named_value(const cJSON *root, const char *field,
                            const named_value_t *table, size_t table_count,
                            uint8_t *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!item || !value || !cJSON_IsString(item) || !item->valuestring)
        return 0;

    for (size_t i = 0U; i < table_count; i++) {
        if (strcmp(item->valuestring, table[i].name) == 0) {
            *value = table[i].value;
            return 1;
        }
    }
    return 0;
}

static int add_i32_array(cJSON *object, const char *name,
                         const int32_t values[MEMORY_MONITOR_RAIL_COUNT])
{
    cJSON *array = cJSON_AddArrayToObject(object, name);
    if (!array) return 0;
    for (size_t i = 0U; i < MEMORY_MONITOR_RAIL_COUNT; i++) {
        cJSON *number = cJSON_CreateNumber(values[i]);
        if (!number || !cJSON_AddItemToArray(array, number)) {
            cJSON_Delete(number);
            return 0;
        }
    }
    return 1;
}

static int add_number_array(cJSON *object, const char *name,
                             const double values[MEMORY_MONITOR_RAIL_COUNT])
{
    cJSON *array = cJSON_AddArrayToObject(object, name);
    if (!array) return 0;
    for (size_t i = 0U; i < MEMORY_MONITOR_RAIL_COUNT; i++) {
        cJSON *number = cJSON_CreateNumber(values[i]);
        if (!number || !cJSON_AddItemToArray(array, number)) {
            cJSON_Delete(number);
            return 0;
        }
    }
    return 1;
}

/* PC主动心跳：只接受空对象{}，返回MCU心跳，不夹带其他状态。 */
static pc_dispatch_result_t handle_client_heartbeat(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    static const uint8_t empty_json[] = {'{', '}'};

    if (root->child != NULL) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }
    return build_response(PC_COMMAND_SERVER_HEARTBEAT, mcu_tx_sequence,
                          empty_json, sizeof(empty_json), response);
}

/* 查询测试仪信息：每次从W25Q256读取有效FRU，再生成三个已冻结字段。 */
static pc_dispatch_result_t handle_query_tester_info(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    fru_info_t fru;
    cJSON *info = NULL;
    char json[192] = {0};
    pc_dispatch_result_t result;

    /* 先校验查询必须为{}，非法请求不能触发Flash访问。 */
    if (root->child != NULL) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }
    if (pc_tester_info_load(&fru) != 0) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "executionFailed", response);
    }

    /* info是本函数创建的响应对象，与借用的请求root分开管理。 */
    info = cJSON_CreateObject();
    if (!info ||
        !cJSON_AddStringToObject(info, "testerName", fru.tester_name) ||
        !cJSON_AddStringToObject(info, "testerVersion", fru.hardware_revision) ||
        !cJSON_AddStringToObject(info, "testBoard", fru.test_board) ||
        !cJSON_PrintPreallocated(info, json, sizeof(json), 0)) {
        cJSON_Delete(info);
        return PC_DISPATCH_ERROR;
    }

    result = build_response(PC_COMMAND_TESTER_INFO_RESP, mcu_tx_sequence,
                            (const uint8_t *)json,
                            (uint32_t)strlen(json), response);
    cJSON_Delete(info);
    return result;
}

/* 只允许MCU发送的响应命令若从PC收到，统一报告方向错误。 */
static pc_dispatch_result_t reject_response_command(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    (void)root;
    return build_command_failure(request, mcu_tx_sequence,
                                 "invalidDirection", response);
}

/* 先完整校验；外部已启动模式交给正式RK业务，其余仍由电源/ready合同门控。 */
static pc_dispatch_result_t handle_start_test(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    pc_test_start_view_t start;
    uint32_t index;
    const uint32_t *valid_index =
        pc_test_start_read_index(root, &index) == PC_TEST_JSON_OK ? &index : NULL;
    const char *error_code = "invalidArgument";

    if (pc_test_start_parse(root, &start) == PC_TEST_JSON_OK) {
#if defined(ENABLE_RK_GATEWAY_TEST) && ENABLE_RK_GATEWAY_TEST
        if (request->command_id == PC_COMMAND_START_TEST && !start.external_ready)
            return rk_gateway_test_start(&start, mcu_tx_sequence, response);
        error_code = "notSupported";
#else
        return pc_test_service_start(request->command_id, &start, mcu_tx_sequence, response);
#endif
    }
    return build_command_failure_with_index(request, mcu_tx_sequence,
                                            error_code, valid_index, response);
}

static pc_dispatch_result_t handle_shmoo_start(
    const pc_protocol_frame_t *request, uint32_t sequence,
    const cJSON *root, pc_dispatch_response_t *response)
{
    (void)request;
    return pc_test_shmoo_start(root, sequence, response);
}

/* 停测、结束整轮或关站已接管OFF，原PC/调试电源命令不能再消费同一事件。 */
static void cancel_power_reply(void)
{
    s_power_command = 0U;
    s_power_owner_connected = 0U;
#if defined(B1_RK_UART)
    if (s_console_power != LOG_CONSOLE_POWER_NONE) {
        log_console_complete_power_request("cancelled");
        s_console_power = LOG_CONSOLE_POWER_NONE;
    }
#endif
}

/* 停止单项执行但不结束整轮；只在真实OFF及RK清理完成后异步回复0006。 */
static pc_dispatch_result_t handle_stop_test(const pc_protocol_frame_t *request,
    uint32_t sequence, const cJSON *root, pc_dispatch_response_t *response)
{
    if (root->child != NULL)
        return build_command_failure(request, sequence, "invalidArgument", response);
    const char *error = pc_test_service_stop();
    if (error) return build_command_failure(request, sequence, error, response);
    cancel_power_reply();
    return PC_DISPATCH_NO_RESPONSE;
}

static pc_dispatch_result_t handle_site_enable(const pc_protocol_frame_t *request,
    uint32_t sequence, const cJSON *root, pc_dispatch_response_t *response)
{
    pc_site_config_t config;
    int deferred;
    if (pc_site_config_parse(root, &config) != PC_TEST_JSON_OK)
        return build_command_failure(request, sequence, "invalidArgument", response);
    const char *error = pc_site_configure(&config, &deferred);
    if (error) return build_command_failure(request, sequence, error, response);
    if (!pc_site_enabled()) cancel_power_reply();
    return deferred ? build_command_accepted(request, sequence, response)
                    : build_command_success(request, sequence, response);
}

/* 整轮命令只在完整校验之后提交业务，避免半张失败清单部分改变灯态。 */
static pc_dispatch_result_t handle_round_begin(const pc_protocol_frame_t *request,
    uint32_t sequence, const cJSON *root, pc_dispatch_response_t *response)
{
    if (root->child != NULL)
        return build_command_failure(request, sequence, "invalidArgument", response);
    return pc_test_round_begin(sequence, response);
}

static pc_dispatch_result_t handle_round_end(const pc_protocol_frame_t *request,
    uint32_t sequence, const cJSON *root, pc_dispatch_response_t *response)
{
    uint32_t failed_mask;
    if (pc_round_end_parse(root, RESULT_LED_TARGET_COUNT, &failed_mask) != PC_TEST_JSON_OK)
        return build_command_failure(request, sequence, "invalidArgument", response);
    pc_dispatch_result_t result = pc_test_round_end(failed_mask, sequence, response);
    if (result == PC_DISPATCH_NO_RESPONSE) cancel_power_reply();
    return result;
}

static pc_dispatch_result_t handle_slt_start(const pc_protocol_frame_t *request,
    uint32_t sequence, const cJSON *root, pc_dispatch_response_t *response)
{
    pc_test_start_view_t start;
    if (pc_slt_start_parse(root, &start) != PC_TEST_JSON_OK)
        return build_command_failure(request, sequence, "invalidArgument", response);
    return pc_test_slt_start(&start, sequence, response);
}

/*
 * 只暴露稳定的业务状态，不把内部等待阶段或PC6/PC7顺序泄漏到线协议。
 * 状态表与pwr_ctrl枚举按下标对应；两个上电等待阶段统一映射为starting。
 */
static const char *const s_power_state_names[] = {
    [PWR_CTRL_STATE_OFF]                  = "off",
    [PWR_CTRL_STATE_WAIT_FIRST_RAIL]      = "starting",
    [PWR_CTRL_STATE_WAIT_SECOND_RAIL]     = "starting",
#if !defined(B1_RK_UART)
    [PWR_CTRL_STATE_WAIT_DDR_READY]       = "starting",
#endif
    [PWR_CTRL_STATE_ON]                   = "on",
#if !defined(B1_RK_UART)
    [PWR_CTRL_STATE_WAIT_DDR_OFF]         = "stopping",
#endif
    [PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF] = "stopping",
    [PWR_CTRL_STATE_FAULT]                = "fault",
};

_Static_assert(sizeof(s_power_state_names) / sizeof(s_power_state_names[0]) ==
               PWR_CTRL_STATE_FAULT + 1U,
               "PC power state table must cover pwr_ctrl states");

static const char *power_state_name(pwr_ctrl_state_t state)
{
    if ((unsigned int)state >=
        sizeof(s_power_state_names) / sizeof(s_power_state_names[0]) ||
        s_power_state_names[state] == NULL) {
        return "fault";
    }
    return s_power_state_names[state];
}

/* 0x0503严格接收{}，返回逻辑状态；它不代表ADC已经确认实际电压。 */
static pc_dispatch_result_t handle_query_power_status(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    cJSON *body = NULL;
    char json[192] = {0};
    pc_dispatch_result_t result;

    if (root->child != NULL) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }

    body = cJSON_CreateObject();
    if (!body ||
        !cJSON_AddStringToObject(body, "powerState",
                                 power_state_name(pc_power_state())) ||
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
        !cJSON_AddNumberToObject(body, "powerCycleCount", v2_power_sim_cycles()) ||
#endif
        !cJSON_PrintPreallocated(body, json, sizeof(json), 0)) {
        cJSON_Delete(body);
        return PC_DISPATCH_ERROR;
    }

    result = build_response(PC_COMMAND_POWER_STATUS_RESP, mcu_tx_sequence,
                            (const uint8_t *)json,
                            (uint32_t)strlen(json), response);
    cJSON_Delete(body);
    return result;
}


/* 先组好受理应答才启动业务。后续0006 completed/failed由next_response发送。 */
#if !defined(ENABLE_V2_POWER_SIMULATION) || !ENABLE_V2_POWER_SIMULATION
static pc_dispatch_result_t start_real_power(const pc_protocol_frame_t *request,
    uint32_t sequence, pc_power_action_t action, pc_dispatch_response_t *response)
{
    int result = build_command_accepted(request, sequence, response);
    if (result != PC_DISPATCH_RESPONSE_READY) return result;
    const char *error = pc_power_service_start(action, NULL, 0U);
    if (error) return build_command_failure(request, sequence, error, response);
    s_power_command = request->command_id;
    s_power_action = action;
    s_power_owner_connected = 1U;
    return result;
}
#endif

static pc_dispatch_result_t handle_power_on(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    pwr_ctrl_state_t previous;

    if (root->child != NULL) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }
    previous = pc_power_state();
    if (previous == PWR_CTRL_STATE_ON)
        return build_command_success(request, mcu_tx_sequence, response);

#if !defined(ENABLE_V2_POWER_SIMULATION) || !ENABLE_V2_POWER_SIMULATION
    return start_real_power(request, mcu_tx_sequence, PC_POWER_ON, response);
#else
    pwr_ctrl_result_t result = pc_power_on(HAL_GetTick());
    if (result == PWR_CTRL_OK)
        return build_command_accepted(request, mcu_tx_sequence, response);
    if (result == PWR_CTRL_ERR_NOT_CONFIGURED)
        return build_command_failure(request, mcu_tx_sequence,
                                     "notConfigured", response);
    if (result == PWR_CTRL_ERR_BUSY ||
        result == PWR_CTRL_ERR_RESOURCE_BUSY) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "busy", response);
    }
    return build_command_failure(request, mcu_tx_sequence,
                                 "executionFailed", response);
#endif
}

/*
 * 下电交给正式编排器直接关轨，不等待RK关机应答；在途互斥由分发入口检查。
 */
static pc_dispatch_result_t handle_power_off(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    pwr_ctrl_state_t state;

    if (root->child != NULL) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }
    state = pc_power_state();
    if (state == PWR_CTRL_STATE_OFF)
        return build_command_success(request, mcu_tx_sequence, response);
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
    v2_power_sim_off();
    return build_command_success(request, mcu_tx_sequence, response);
#else
    return start_real_power(request, mcu_tx_sequence, PC_POWER_OFF, response);
#endif
}

/* 冷重启保留训练目标和当前Boot设置；直接关电、等待、上电和本轮心跳确认逐步推进。 */
static pc_dispatch_result_t handle_reset_rk(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{

    if (root->child != NULL) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
    pwr_ctrl_state_t state = pc_power_state();
    if (state == PWR_CTRL_STATE_ON) {
        v2_power_sim_restart();
        return build_command_success(request, mcu_tx_sequence, response);
    }
    if (state == PWR_CTRL_STATE_OFF)
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidState", response);
    if (state == PWR_CTRL_STATE_FAULT)
        return build_command_failure(request, mcu_tx_sequence,
                                     "executionFailed", response);
    return build_command_failure(request, mcu_tx_sequence,
                                 "busy", response);
#else
    return start_real_power(request, mcu_tx_sequence, PC_POWER_RESTART, response);
#endif
}

static const named_value_t s_boot_mode_values[] = {
    {"usbMaskrom", BOOT_MODE_USB_MASKROM},
    {"sdCard",     BOOT_MODE_SD_CARD},
    {"emmc",       BOOT_MODE_EMMC},
    {"fspiM0",     BOOT_MODE_FSPI_M0},
    {"fspiM1",     BOOT_MODE_FSPI_M1},
    {"fspiM2",     BOOT_MODE_FSPI_M2},
    {"emmcSd",     BOOT_MODE_EMMC_SD},
};

static const char *const s_boot_mode_names[] = {
    [BOOT_MODE_USB_MASKROM] = "usbMaskrom",
    [BOOT_MODE_SD_CARD]     = "sdCard",
    [BOOT_MODE_EMMC]        = "emmc",
    [BOOT_MODE_FSPI_M0]     = "fspiM0",
    [BOOT_MODE_FSPI_M1]     = "fspiM1",
    [BOOT_MODE_FSPI_M2]     = "fspiM2",
    [BOOT_MODE_EMMC_SD]     = "emmcSd",
};

_Static_assert(sizeof(s_boot_mode_names) / sizeof(s_boot_mode_names[0]) ==
               BOOT_MODE_COUNT, "PC Boot mode table must cover boot_mode_t");

static pc_dispatch_result_t handle_set_boot_mode(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    static const char *const fields[] = {"bootMode"};
    monitor_boot_result_t result;
    uint8_t mode;

    if (!object_has_exact_fields(root, fields,
                                 sizeof(fields) / sizeof(fields[0])) ||
        !read_named_value(root, "bootMode", s_boot_mode_values,
                          sizeof(s_boot_mode_values) /
                          sizeof(s_boot_mode_values[0]), &mode)) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }

    result = pc_boot_set((boot_mode_t)mode);
    /* 参数/状态/锁忙拒绝未写DAC；成功或不确定的写失败才使旧启动配置失效。 */
    if (result != MONITOR_BOOT_ERR_PARAM && result != MONITOR_BOOT_ERR_STATE &&
        result != MONITOR_BOOT_ERR_BUSY)
        pc_power_service_invalidate();
    if (result == MONITOR_BOOT_OK)
        return build_command_success(request, mcu_tx_sequence, response);
    if (result == MONITOR_BOOT_ERR_STATE)
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidState", response);
    if (result == MONITOR_BOOT_ERR_BUSY)
        return build_command_failure(request, mcu_tx_sequence,
                                     "busy", response);
    if (result == MONITOR_BOOT_ERR_PARAM)
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    return build_command_failure(request, mcu_tx_sequence,
                                 "executionFailed", response);
}

static pc_dispatch_result_t handle_query_boot_mode(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    monitor_boot_result_t load_result;
    boot_mode_t mode;
    uint16_t target_mv;
    cJSON *body = NULL;
    char json[224] = {0};
    pc_dispatch_result_t result;

    if (root->child != NULL) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }
    load_result = pc_boot_get(&mode, &target_mv);
    if (load_result == MONITOR_BOOT_ERR_NOT_SET) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "notConfigured", response);
    }
    if (load_result == MONITOR_BOOT_ERR_BUSY) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "busy", response);
    }
    if (load_result != MONITOR_BOOT_OK || mode >= BOOT_MODE_COUNT) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "executionFailed", response);
    }

    body = cJSON_CreateObject();
    if (!body ||
        !cJSON_AddStringToObject(body, "bootMode", s_boot_mode_names[mode]) ||
        !cJSON_AddNumberToObject(body, "targetMv", target_mv) ||
        !cJSON_PrintPreallocated(body, json, sizeof(json), 0)) {
        cJSON_Delete(body);
        return PC_DISPATCH_ERROR;
    }
    result = build_response(PC_COMMAND_BOOT_MODE_RESP, mcu_tx_sequence,
                            (const uint8_t *)json,
                            (uint32_t)strlen(json), response);
    cJSON_Delete(body);
    return result;
}

/*
 * 0x0509固定memoryType及字符串mV的vdd1/vdd2/vddq，与0103共用精确编码校验。
 * B1在OFF时只保存下次上电目标；ON时同类型立即调压，不覆盖已保存的训练目标。
 */
static pc_dispatch_result_t handle_set_ddr_config(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    int16_t steps[DDR_RAIL_COUNT];
    uint16_t target_mv[DDR_RAIL_COUNT];
    const char *memory_type;
    pwr_ctrl_result_t result;

    if (pc_power_parse(root, &memory_type, steps, target_mv) != PC_TEST_JSON_OK) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }

    ddr_memory_type_t type = strcmp(memory_type, "LPDDR4") == 0 ? DDR_MEMORY_LPDDR4 : DDR_MEMORY_LPDDR4X;
    result = pc_ddr_set(type, steps);
    /* 明确未执行的拒绝保留标记；成功、故障及未知返回可能已改变供电条件。 */
    if (result != PWR_CTRL_ERR_PARAM && result != PWR_CTRL_ERR_NOT_CONFIGURED &&
        result != PWR_CTRL_ERR_BUSY && result != PWR_CTRL_ERR_RESOURCE_BUSY)
        pc_power_service_invalidate();
    if (result == PWR_CTRL_OK)
        return build_command_success(request, mcu_tx_sequence, response);
    if (result == PWR_CTRL_ERR_PARAM)
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    if (result == PWR_CTRL_ERR_BUSY)
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidState", response);
    if (result == PWR_CTRL_ERR_RESOURCE_BUSY)
        return build_command_failure(request, mcu_tx_sequence,
                                     "busy", response);
    return build_command_failure(request, mcu_tx_sequence,
                                 "executionFailed", response);
}

/* 0x0601只复制监测任务发布的快照，TCP任务绝不直接访问PAC/TPS/AD。 */
static pc_dispatch_result_t handle_query_power_monitor(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    monitor_snapshot_t snapshot;
    cJSON *body = NULL;
    char json[PC_DISPATCH_RESPONSE_CAPACITY - PC_PROTOCOL_HEADER_SIZE] = {0};
    uint8_t valid_mask;
    double current_ma[MEMORY_MONITOR_RAIL_COUNT];
    double power_mw[MEMORY_MONITOR_RAIL_COUNT];
    pc_dispatch_result_t result;

    if (root->child != NULL) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }
    if (!pc_sample_get(&snapshot)) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "executionFailed", response);
    }

    /* 首版只公开实际电压及IDD/功率；TPS STATUS是读清事件，暂不伪装成健康态。 */
    valid_mask = snapshot.memory.valid_mask &
                 (MEMORY_MONITOR_VALID_VOLTAGE | MEMORY_MONITOR_VALID_IDD);
    body = cJSON_CreateObject();
    if (!body ||
        !cJSON_AddNumberToObject(body, "sampleAgeMs",
                                 (uint32_t)(HAL_GetTick() -
                                            snapshot.timestamp_ms)) ||
        !cJSON_AddStringToObject(body, "samplePowerState",
                                 power_state_name(snapshot.power_state)) ||
        !cJSON_AddNumberToObject(body, "validMask", valid_mask)) {
        cJSON_Delete(body);
        return PC_DISPATCH_ERROR;
    }

    /* 未置位的整组字段直接省略，不能把清零缓冲冒充0V或0mA实测值。 */
    if ((valid_mask & MEMORY_MONITOR_VALID_VOLTAGE) != 0U &&
        !add_i32_array(body, "voltageMv", snapshot.memory.voltage_mv)) {
        cJSON_Delete(body);
        return PC_DISPATCH_ERROR;
    }
    if ((valid_mask & MEMORY_MONITOR_VALID_IDD) != 0U) {
        for (size_t rail = 0U; rail < MEMORY_MONITOR_RAIL_COUNT; rail++) {
            /* 内部保留µA，只在JSON边界换成mA数值，避免提前舍掉小电流。 */
            current_ma[rail] = (double)snapshot.memory.idd[rail].current_ua / 1000.0;
            power_mw[rail] = snapshot.memory.idd[rail].power_mw;
        }
        if (!add_number_array(body, "currentMa", current_ma) ||
            !add_number_array(body, "powerMw", power_mw)) {
            cJSON_Delete(body);
            return PC_DISPATCH_ERROR;
        }
    }

#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
    if (!v2_power_sim_add_statistics(body)) {
        cJSON_Delete(body);
        return PC_DISPATCH_ERROR;
    }
#elif defined(B1_RK_UART)
    idd_stats_t stats;
    if (!monitor_idd_get(&stats) || !pc_test_add_statistics(body, &stats)) {
        cJSON_Delete(body);
        return PC_DISPATCH_ERROR;
    }
#endif
    if (!cJSON_PrintPreallocated(body, json, sizeof(json), 0)) {
        cJSON_Delete(body);
        return PC_DISPATCH_ERROR;
    }
    result = build_response(PC_COMMAND_POWER_MONITOR_RESP, mcu_tx_sequence,
                            (const uint8_t *)json,
                            (uint32_t)strlen(json), response);
    cJSON_Delete(body);
    return result;
}


/*
 * PC命令表：新增命令时编写一个处理函数，并在此添加一项，CommandID不可重复。
 * 当前合同按CommandID查找；ServiceID已在帧状态机固定校验为0。
 * 命令集合在编译时确定，使用只读表，无需启动注册或额外RAM登记表。
 * 反向命令显式返回invalidDirection；未登记命令返回notSupported。
 */
static const pc_command_entry_t s_command_table[] = {
    {PC_COMMAND_SERVER_HEARTBEAT,  reject_response_command}, /* MCU -> PC，拒收。 */
    {PC_COMMAND_CLIENT_HEARTBEAT,  handle_client_heartbeat},  /* PC -> MCU，回复{}。 */
    {PC_COMMAND_COMMAND_RESP,      reject_response_command}, /* MCU -> PC，拒收。 */
    {PC_COMMAND_QUERY_TESTER_INFO, handle_query_tester_info}, /* PC -> MCU，读取FRU。 */
    {PC_COMMAND_TESTER_INFO_RESP,  reject_response_command}, /* MCU -> PC，拒收。 */
    {PC_COMMAND_START_TEST,        handle_start_test},        /* 轮内FUNC，最终上报0105。 */
    {PC_COMMAND_STOP_TEST,         handle_stop_test},         /* 中止执行并关RK，保留整轮。 */
    {PC_COMMAND_TEST_RESULT_REPORT, reject_response_command}, /* MCU→PC，拒收。 */
    {PC_COMMAND_ROUND_BEGIN,       handle_round_begin},       /* 建立当前连接的整轮。 */
    {PC_COMMAND_ROUND_END,         handle_round_end},         /* 汇总灯、停止当前测试并下电。 */
    {PC_COMMAND_PARA_START,        handle_start_test},        /* 轮内PARA，最终上报0109。 */
    {PC_COMMAND_PARA_RESULT,       reject_response_command},  /* MCU→PC，拒收。 */
    {PC_COMMAND_SLT_START,         handle_slt_start},         /* 电源/Boot准备，不等待RK。 */
    {PC_COMMAND_SITE_ENABLE,       handle_site_enable},      /* 站点开关与两镜像DUT灯，不受算法busy限制。 */
    {PC_COMMAND_CHIP_INFO,         reject_response_command}, /* 010D仅由MCU主动上报。 */
    {PC_COMMAND_SHMOO_START,       handle_shmoo_start},
    {PC_COMMAND_SHMOO_RESULT,      reject_response_command},
    {PC_COMMAND_POWER_ON,          handle_power_on},          /* 非阻塞上电请求。 */
    {PC_COMMAND_POWER_OFF,         handle_power_off},         /* 直接关电，仍须通过在途互斥。 */
    {PC_COMMAND_QUERY_POWER_STATUS, handle_query_power_status}, /* PC -> MCU，只读。 */
    {PC_COMMAND_POWER_STATUS_RESP, reject_response_command},  /* MCU -> PC，拒收。 */
    {PC_COMMAND_RESET_RK,          handle_reset_rk},          /* 冷重启并等待普通心跳。 */
    {PC_COMMAND_SET_BOOT_MODE,     handle_set_boot_mode},     /* OFF状态写Boot DAC。 */
    {PC_COMMAND_QUERY_BOOT_MODE,   handle_query_boot_mode},   /* 查询已配置模式。 */
    {PC_COMMAND_BOOT_MODE_RESP,    reject_response_command},  /* MCU -> PC，拒收。 */
    {PC_COMMAND_SET_DDR_CONFIG, handle_set_ddr_config},       /* 选择类型/三轨拉偏。 */
    {PC_COMMAND_QUERY_POWER_MONITOR, handle_query_power_monitor}, /* 读取缓存快照。 */
    {PC_COMMAND_POWER_MONITOR_RESP, reject_response_command}, /* MCU -> PC，拒收。 */
};

pc_dispatch_result_t pc_dispatch_handle(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    pc_dispatch_response_t *response)
{
    cJSON *root = NULL;
    pc_dispatch_result_t result = PC_DISPATCH_NO_RESPONSE;
    uint8_t command_matched = 0U;

    if (!request || !response) return PC_DISPATCH_ERROR;
    /* 即使本次不响应或报错，也不能让TCP任务误发上一次留下的PDU。 */
    response->length = 0U;

    /* 所有命令（包括未知命令）都先经过一次严格JSON对象校验。 */
    pc_json_result_t json_result = pc_json_parse_object(
        request->json, request->json_length, &root, NULL);
    if (json_result != PC_JSON_OK) {
        /*
         * FUNC/PARA重复键按业务参数错误应答；共享入口已经验证UTF-8、词法、语法和
         * 对象边界，仅因重复键释放树。此错误分支重新取树，只提取唯一合法index，
         * 绝不执行命令；其他命令的重复键统一按invalidArgument应答。
         */
        if ((request->command_id == PC_COMMAND_START_TEST || request->command_id == PC_COMMAND_PARA_START) &&
            json_result == PC_JSON_ERROR_DUPLICATE_KEY) {
            root = cJSON_ParseWithLengthOpts((const char *)request->json,
                                             request->json_length, NULL, 0);
            if (root) {
                uint32_t index;
                const uint32_t *valid_index =
                    pc_test_start_read_index(root, &index) == PC_TEST_JSON_OK ? &index : NULL;
                result = build_command_failure_with_index(request, mcu_tx_sequence,
                                                           "invalidArgument", valid_index, response);
                cJSON_Delete(root);
                return result;
            }
        }
        /* 完整帧边界由状态机提供，JSON失败不影响下一帧；不得调用任何业务。 */
        return build_command_failure(request, mcu_tx_sequence, "invalidArgument", response);
    }

    if (!pc_site_enabled() && request->command_id != PC_COMMAND_SITE_ENABLE &&
        request->command_id != PC_COMMAND_CLIENT_HEARTBEAT) {
        uint32_t index;
        const uint32_t *valid_index = NULL;
        if ((request->command_id == PC_COMMAND_START_TEST || request->command_id == PC_COMMAND_PARA_START) &&
            pc_test_start_read_index(root, &index) == PC_TEST_JSON_OK)
            valid_index = &index;
        cJSON_Delete(root);
        return build_command_failure_with_index(request, mcu_tx_sequence, "siteDisabled", valid_index, response);
    }

    /* 算法在途或超时后执行状态未知时，禁止会改变运行环境的业务。
     * 查询/心跳仍可用，不影响正在执行的算法。
     */
    static const uint16_t test_conflicts[] = {
        PC_COMMAND_POWER_ON, PC_COMMAND_POWER_OFF, PC_COMMAND_RESET_RK,
        PC_COMMAND_SET_BOOT_MODE, PC_COMMAND_SET_DDR_CONFIG,
    };
    int setting = 0;
#if defined(B1_RK_UART)
    setting = rk_task_set_pending();
    if (setting && (request->command_id == PC_COMMAND_START_TEST || request->command_id == PC_COMMAND_PARA_START ||
                    request->command_id == PC_COMMAND_SLT_START || request->command_id == PC_COMMAND_SHMOO_START)) {
        uint32_t index;
        const uint32_t *valid_index = (request->command_id == PC_COMMAND_START_TEST || request->command_id == PC_COMMAND_PARA_START) &&
            pc_test_start_read_index(root, &index) == PC_TEST_JSON_OK ? &index : NULL;
        cJSON_Delete(root);
        return build_command_failure_with_index(request, mcu_tx_sequence, "busy", valid_index, response);
    }
#endif
    if (setting || pc_test_service_busy() || pc_power_service_busy()) {
        int conflict = 0;
        for (size_t i = 0U; i < sizeof(test_conflicts) / sizeof(test_conflicts[0]); ++i)
            if (request->command_id == test_conflicts[i]) conflict = 1;
        if (conflict) {
            cJSON_Delete(root);
            return build_command_failure(request, mcu_tx_sequence, "busy", response);
        }
    }

    /* 手动交接期间保留查询；ARMED可0501/Boot/电压启动，0505仍拒绝以免等SPL心跳。 */
    if (handoff_mcu_busy()) {
        int conflict = request->command_id == PC_COMMAND_RESET_RK;
        for (size_t i = 0U; i < sizeof(test_conflicts) / sizeof(test_conflicts[0]); ++i)
            if (request->command_id == test_conflicts[i] &&
                !handoff_mcu_power_allowed(request->command_id == PC_COMMAND_POWER_OFF)) conflict = 1;
        if (conflict) {
            cJSON_Delete(root);
            return build_command_failure(request, mcu_tx_sequence, "busy", response);
        }
    }

    /* 小表顺序查找，命中只执行一次；未命中返回notSupported，不隐式转发RK。 */
    for (size_t i = 0U; i < sizeof(s_command_table) / sizeof(s_command_table[0]); i++) {
        if (s_command_table[i].command_id == request->command_id) {
            command_matched = 1U;
            result = s_command_table[i].handler(request, mcu_tx_sequence,
                                                root, response);
            break;
        }
    }

    if (!command_matched) {
        result = build_command_failure(request, mcu_tx_sequence,
                                       "notSupported", response);
    }

    /* 统一收尾：处理函数无论成功、拒绝还是不响应，请求JSON都只释放一次。 */
    cJSON_Delete(root);
    return result;
}

#if defined(B1_RK_UART)
const char *pc_dispatch_debug_set(const char *name, int64_t value)
{
    if (!name) return "invalidArgument";
    if (!pc_site_enabled()) return "siteDisabled";
    if (strcmp(name, "Freq") == 0 || strcmp(name, "freq") == 0) {
        if (value < -1) return "invalidArgument";
        s_next_manual_frequency = value;
        return NULL;
    }
    int index = pc_timing_field_index(name);
    if (index < 0) return "invalidTiming";
    if (value < 0 || value > UINT32_MAX) return "invalidArgument";
    if (pc_test_service_busy() || pc_power_service_busy() || s_power_command || handoff_mcu_busy()) return "busy";
    const char *error = rk_task_set_timing(pc_timing_command((unsigned)index), (uint32_t)value);
    /* 写入可能改变当前RK配置，下一次正式带电源参数的测试须重新准备。 */
    if (!error) pc_power_service_invalidate();
    return error;
}

/* 本地串口与PC共用唯一TCP业务上下文，避免跨任务检查busy后再操作电源的竞争。
 * 只复用PREPARE_SLT的冷启动/Boot步骤，不建立SLT测试占用，也不等待RK心跳。
 */
static void poll_console_power(void)
{
    const char *error;
    if (s_console_power != LOG_CONSOLE_POWER_NONE) {
        pc_power_action_t action = s_console_power == LOG_CONSOLE_POWER_END_RK ? PC_POWER_OFF : PC_POWER_PREPARE_SLT;
        if (pc_power_service_take(action, &error)) {
            log_console_complete_power_request(error);
            s_console_power = LOG_CONSOLE_POWER_NONE;
        }
        return;
    }
    log_console_power_request_t request = log_console_take_power_request();
    if (request == LOG_CONSOLE_POWER_NONE) return;
    if (!pc_site_enabled()) {
        log_console_complete_power_request("siteDisabled");
        return;
    }
    if (!handoff_mcu_power_allowed(request == LOG_CONSOLE_POWER_END_RK)) {
        log_console_complete_power_request("busy");
        return;
    }
    if (pc_test_service_busy() || pc_power_service_busy() || s_power_command || rk_task_set_pending()) {
        log_console_complete_power_request("busy");
        return;
    }
    pc_power_target_t target = {
        .memory_type = request == LOG_CONSOLE_POWER_START_LPDDR4 ? DDR_MEMORY_LPDDR4 : DDR_MEMORY_LPDDR4X,
        .boot_mode = BOOT_MODE_FSPI_M1,
        .manual_boot = 1U,
        .manual_frequency = s_next_manual_frequency,
    };
    int starting = request != LOG_CONSOLE_POWER_END_RK;
    const int16_t nominal[DDR_RAIL_COUNT] = {0};
    if (starting && ddr_power_build_step_targets(target.memory_type, nominal, target.mv) != IF_OK) {
        log_console_complete_power_request("invalidArgument");
        return;
    }
    error = pc_power_service_start(starting ? PC_POWER_PREPARE_SLT : PC_POWER_OFF, starting ? &target : NULL, 1U);
    if (error) log_console_complete_power_request(error);
    else {
        if (starting) s_next_manual_frequency = -1;
        s_console_power = request;
    }
}
#endif

#if defined(B1_RK_UART)
/* 调试文字命令在业务所属任务执行，回复直接送回原调试端。 */
static void handle_console_debug(char *command, char *reply, size_t capacity)
{
    if (strcmp(command, "test status") == 0) {
        pc_test_format_status(reply, capacity);
        return;
    }
    char *name = command + 4U; /* 控制台只交付完整的set行或test status。 */
    char *number = strchr(name, ' ');
    int64_t value;
    if (number) *number++ = '\0';
    int frequency = strcmp(name, "freq") == 0 || strcmp(name, "Freq") == 0;
    if (!number || !*name || !pc_timing_integer(number, frequency, &value) || value < -1) {
        (void)snprintf(reply, capacity, "\r\n[RK TIMING] invalidArgument; use: set <timing|freq> <decimal>\r\n");
        return;
    }
    const char *error = pc_dispatch_debug_set(name, value);
    const char *status = error ? error : (frequency ? "next manual start" : "queued");
    (void)snprintf(reply, capacity, "\r\n[RK TIMING] set %s %s %s\r\n", name, number, status);
}
#endif

void pc_dispatch_poll(void)
{
#if defined(B1_RK_UART)
    /* 先处理人工中止，避免本次轮询继续执行已经到期的上电步骤。 */
    if (log_console_take_force_off()) {
        const char *error = pc_test_service_stop_console();
        if (error) log_console_complete_force_off(error);
        else cancel_power_reply();
    }
#endif
    pc_power_service_poll();
    if (s_power_command && !s_power_owner_connected) {
        const char *error;
        if (pc_power_service_take(s_power_action, &error)) s_power_command = 0U;
    }
#if defined(B1_RK_UART)
    log_console_service_debug(handle_console_debug);
    poll_console_power();
    char name[LOG_CONSOLE_FUNC_NAME_SIZE];
    if (log_console_take_func(name)) {
        const char *error = s_power_command || s_console_power != LOG_CONSOLE_POWER_NONE
                                ? "busy" : pc_test_service_start_console_func(name);
        if (error) {
            char reply[128];
            const char *hint = strcmp(error, "powerOff") == 0 ? "; use start rk <LPDDR4|LPDDR4X>" :
                               strcmp(error, "notReady") == 0 ? "; check SPL ready" : "";
            (void)snprintf(reply, sizeof(reply), "\r\n[func] %s failed: %s%s\r\n", name, error, hint);
            log_console_complete_func(reply);
        }
    }
#endif
#if defined(ENABLE_V2_POWER_SIMULATION) && ENABLE_V2_POWER_SIMULATION
    v2_power_sim_poll();
#endif
    /* 无连接时仍回收旧测试结果；有连接的响应留给next_response统一发送。 */
#if !defined(ENABLE_RK_GATEWAY_TEST) || !ENABLE_RK_GATEWAY_TEST
    (void)pc_test_service_poll(0U, NULL);
#endif
}

int pc_dispatch_next_response(uint32_t sequence, pc_dispatch_response_t *response)
{
    const char *error;
    if (response && pc_site_take_reply(&error)) {
        pc_protocol_frame_t request = {.command_id = PC_COMMAND_SITE_ENABLE};
        return error ? build_command_failure(&request, sequence, error, response)
                     : build_command_success(&request, sequence, response);
    }
    if (s_power_command && s_power_owner_connected && response &&
        pc_power_service_take(s_power_action, &error)) {
        pc_protocol_frame_t request = {.command_id = s_power_command};
        s_power_command = 0U;
        return error ? build_command_failure(&request, sequence, error, response)
                     : build_command_success(&request, sequence, response);
    }
#if defined(ENABLE_RK_GATEWAY_TEST) && ENABLE_RK_GATEWAY_TEST
    /* 旧隔离镜像仍只有一个RK结果消费者，不能与正式业务同时抢队列。 */
    return rk_gateway_test_poll(sequence, response);
#else
    return pc_test_service_poll(sequence, response);
#endif
}

void pc_dispatch_response_sent(uint16_t command)
{
    if (command == PC_COMMAND_CHIP_INFO) pc_test_chip_info_sent();
    if (command == PC_COMMAND_SHMOO_RESULT) pc_test_shmoo_point_sent();
}

void pc_dispatch_disconnected(void)
{
    s_power_owner_connected = 0U;
    pc_site_disconnected();
    pc_test_service_disconnected();
#if defined(ENABLE_RK_GATEWAY_TEST) && ENABLE_RK_GATEWAY_TEST
    rk_gateway_test_disconnect();
#endif
}
