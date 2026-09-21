#include "pc_dispatch.h"

#include "pc_command.h"
#include "pc_test_message.h"
#include "pc_tester_info.h"
#include "monitor_task.h"
#include "pwr_ctrl.h"
#include "result_led.h"
#include "ota_app.h"
#include "pc_ota.h"

#include <string.h>

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
    return pc_protocol_build_response(response_command_id, mcu_tx_sequence,
                                      json, json_length,
                                      response->data, sizeof(response->data),
                                      &response->length) == 0
               ? PC_DISPATCH_RESPONSE_READY
               : PC_DISPATCH_ERROR;
}

/*
 * 已通过帧头和严格JSON语法校验后的业务错误统一使用0x0006返回，不能把
 * 参数错误升级为TCP断连。失败固定status=failed；同步控制完成统一返回
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
    char json[160] = {0};
    pc_dispatch_result_t result;

    if (!request || !error_code) return PC_DISPATCH_ERROR;
    body = cJSON_CreateObject();
    if (!body ||
        !cJSON_AddNumberToObject(body, "requestCommandId", request->command_id) ||
        !cJSON_AddStringToObject(body, "status", "failed") ||
        !cJSON_AddStringToObject(body, "errorCode", error_code) ||
        (algorithm_index && !cJSON_AddNumberToObject(body, "algorithmIndex", *algorithm_index)) ||
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
    char json[80] = {0};
    pc_dispatch_result_t result;

    if (!request || !status) return PC_DISPATCH_ERROR;
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

static int read_i16(const cJSON *root, const char *name, int16_t *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    double number;
    int16_t integer;

    if (!item || !value || !cJSON_IsNumber(item)) return 0;
    number = item->valuedouble;
    if (number < INT16_MIN || number > INT16_MAX) return 0;
    integer = (int16_t)number;
    if ((double)integer != number) return 0;
    *value = integer;
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

static int add_u32_array(cJSON *object, const char *name,
                         const uint32_t values[MEMORY_MONITOR_RAIL_COUNT])
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

/* 完整校验启动请求，但RK执行与安全合同未接通前拒绝执行，绝不假报受理成功。 */
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
        /* 这里只做纯计算；不选Boot、不改训练目标、不上电，也不投递RK。 */
        error_code = ota_app_is_confirmed() ? "notReady" : "otaNotReady";
    }
    return build_command_failure_with_index(request, mcu_tx_sequence,
                                            error_code, valid_index, response);
}

/*
 * 只暴露稳定的业务状态，不把内部等待阶段或PC6/PC7顺序泄漏到线协议。
 * 状态表与pwr_ctrl枚举按下标对应；两个上电等待阶段统一映射为starting。
 */
static const char *const s_power_state_names[] = {
    [PWR_CTRL_STATE_OFF]                  = "off",
    [PWR_CTRL_STATE_WAIT_FIRST_RAIL]      = "starting",
    [PWR_CTRL_STATE_WAIT_SECOND_RAIL]     = "starting",
    [PWR_CTRL_STATE_ON]                   = "on",
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
    char json[64] = {0};
    pc_dispatch_result_t result;

    if (root->child != NULL) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }

    body = cJSON_CreateObject();
    if (!body ||
        !cJSON_AddStringToObject(body, "powerState",
                                 power_state_name(pwr_ctrl_get_state())) ||
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


static pc_dispatch_result_t handle_power_on(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    pwr_ctrl_state_t previous;
    pwr_ctrl_result_t result;

    if (root->child != NULL) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }
    previous = pwr_ctrl_get_state();
    if (previous == PWR_CTRL_STATE_ON)
        return build_command_success(request, mcu_tx_sequence, response);

    result = monitor_request_power_on(HAL_GetTick());
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
}

/*
 * 正常下电必须先取得RK安全关机确认。第1项RK业务冻结前，ON状态明确返回
 * notReady，绝不能为了接入命令而直接调用底层关轨接口。
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
    state = pwr_ctrl_get_state();
    if (state == PWR_CTRL_STATE_OFF)
        return build_command_success(request, mcu_tx_sequence, response);
    if (state == PWR_CTRL_STATE_ON)
        return build_command_failure(request, mcu_tx_sequence,
                                     "notReady", response);
    if (state == PWR_CTRL_STATE_FAULT)
        return build_command_failure(request, mcu_tx_sequence,
                                     "executionFailed", response);
    return build_command_failure(request, mcu_tx_sequence,
                                 "busy", response);
}

/* RK电源循环同样依赖安全关机确认；当前先冻结入口和错误语义。 */
static pc_dispatch_result_t handle_reset_rk(
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
    state = pwr_ctrl_get_state();
    if (state == PWR_CTRL_STATE_ON)
        return build_command_failure(request, mcu_tx_sequence,
                                     "notReady", response);
    if (state == PWR_CTRL_STATE_OFF)
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidState", response);
    if (state == PWR_CTRL_STATE_FAULT)
        return build_command_failure(request, mcu_tx_sequence,
                                     "executionFailed", response);
    return build_command_failure(request, mcu_tx_sequence,
                                 "busy", response);
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

    result = monitor_set_boot_mode((boot_mode_t)mode);
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
    char json[96] = {0};
    pc_dispatch_result_t result;

    if (root->child != NULL) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }
    load_result = monitor_get_boot_mode(&mode, &target_mv);
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

static const named_value_t s_memory_type_values[] = {
    {"LPDDR4",  DDR_MEMORY_LPDDR4},
    {"LPDDR4X", DDR_MEMORY_LPDDR4X},
};

/*
 * 0x0509固定四字段：三路steps都是相对所选内存标称值的有符号20mV档位。
 * OFF+全零只选择类型；ON+相同类型才执行调压，不缓存下次上电的拉偏值。
 */
static pc_dispatch_result_t handle_set_ddr_config(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    static const char *const fields[] = {
        "memoryType", "vdd1Steps", "vdd2Steps", "vddqSteps"
    };
    int16_t steps[DDR_RAIL_COUNT];
    uint8_t memory_type;
    pwr_ctrl_result_t result;

    if (!object_has_exact_fields(root, fields,
                                 sizeof(fields) / sizeof(fields[0])) ||
        !read_named_value(root, "memoryType", s_memory_type_values,
                          sizeof(s_memory_type_values) /
                          sizeof(s_memory_type_values[0]), &memory_type) ||
        !read_i16(root, "vdd1Steps", &steps[DDR_RAIL_VDD1]) ||
        !read_i16(root, "vdd2Steps", &steps[DDR_RAIL_VDD2]) ||
        !read_i16(root, "vddqSteps", &steps[DDR_RAIL_VDDQ])) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }

    result = monitor_apply_ddr_config((ddr_memory_type_t)memory_type, steps);
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
    char json[320] = {0};
    uint8_t valid_mask;
    uint32_t current_ma[MEMORY_MONITOR_RAIL_COUNT];
    uint32_t power_mw[MEMORY_MONITOR_RAIL_COUNT];
    pc_dispatch_result_t result;

    if (root->child != NULL) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }
    if (!monitor_get_latest(&snapshot)) {
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
            current_ma[rail] = snapshot.memory.idd[rail].current_ma;
            power_mw[rail] = snapshot.memory.idd[rail].power_mw;
        }
        if (!add_u32_array(body, "currentMa", current_ma) ||
            !add_u32_array(body, "powerMw", power_mw)) {
            cJSON_Delete(body);
            return PC_DISPATCH_ERROR;
        }
    }

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

static const named_value_t s_led_target_values[] = {
    {"lpddr0", RESULT_LED_LPDDR0},
    {"lpddr1", RESULT_LED_LPDDR1},
    {"emcp",   RESULT_LED_EMCP},
};

static const named_value_t s_led_state_values[] = {
    {"off",        RESULT_LED_OFF},
    {"pass",       RESULT_LED_PASS},
    {"fail",       RESULT_LED_FAIL},
    {"testNormal", RESULT_LED_TEST_NORMAL},
    {"testHot",    RESULT_LED_TEST_HOT},
};

static const char *const s_led_state_names[] = {
    [RESULT_LED_OFF]         = "off",
    [RESULT_LED_PASS]        = "pass",
    [RESULT_LED_FAIL]        = "fail",
    [RESULT_LED_TEST_NORMAL] = "testNormal",
    [RESULT_LED_TEST_HOT]    = "testHot",
};

_Static_assert(sizeof(s_led_state_names) / sizeof(s_led_state_names[0]) ==
               RESULT_LED_TEST_HOT + 1U,
               "PC LED state table must cover result_led states");

/* 设置业务对象，不在线协议暴露物理颜色、GPIO或LED编号。 */
static pc_dispatch_result_t handle_set_result_indicator(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    static const char *const fields[] = {"target", "state"};
    uint8_t target;
    uint8_t state;

    if (!object_has_exact_fields(root, fields,
                                 sizeof(fields) / sizeof(fields[0])) ||
        !read_named_value(root, "target", s_led_target_values,
                          sizeof(s_led_target_values) /
                          sizeof(s_led_target_values[0]), &target) ||
        !read_named_value(root, "state", s_led_state_values,
                          sizeof(s_led_state_values) /
                          sizeof(s_led_state_values[0]), &state)) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }

    if (result_led_set((result_led_target_t)target,
                       (result_led_state_t)state) != HAL_OK) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "executionFailed", response);
    }
    return build_command_success(request, mcu_tx_sequence, response);
}

static pc_dispatch_result_t handle_query_indicator_status(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    result_led_state_t states[RESULT_LED_TARGET_COUNT];
    cJSON *body = NULL;
    char json[128] = {0};
    pc_dispatch_result_t result;

    if (root->child != NULL) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "invalidArgument", response);
    }
    if (result_led_get_all(states) != HAL_OK) {
        return build_command_failure(request, mcu_tx_sequence,
                                     "executionFailed", response);
    }

    body = cJSON_CreateObject();
    if (!body ||
        !cJSON_AddStringToObject(body, "lpddr0",
                                 s_led_state_names[states[RESULT_LED_LPDDR0]]) ||
        !cJSON_AddStringToObject(body, "lpddr1",
                                 s_led_state_names[states[RESULT_LED_LPDDR1]]) ||
        !cJSON_AddStringToObject(body, "emcp",
                                 s_led_state_names[states[RESULT_LED_EMCP]]) ||
        !cJSON_PrintPreallocated(body, json, sizeof(json), 0)) {
        cJSON_Delete(body);
        return PC_DISPATCH_ERROR;
    }

    result = build_response(PC_COMMAND_INDICATOR_STATUS_RESP, mcu_tx_sequence,
                            (const uint8_t *)json,
                            (uint32_t)strlen(json), response);
    cJSON_Delete(body);
    return result;
}

/*
 * PC命令表：新增命令时编写一个处理函数，并在此添加一项，CommandID不可重复。
 * 当前合同按CommandID查找；ServiceID已在帧状态机固定校验为0。
 * 命令集合在编译时确定，使用只读表，无需启动注册或额外RAM登记表。
 * 明确列出已冻结的反向命令，避免它们落入“未知命令暂不响应”的路径。
 */
static const pc_command_entry_t s_command_table[] = {
    {PC_COMMAND_SERVER_HEARTBEAT,  reject_response_command}, /* MCU -> PC，拒收。 */
    {PC_COMMAND_CLIENT_HEARTBEAT,  handle_client_heartbeat},  /* PC -> MCU，回复{}。 */
    {PC_COMMAND_COMMAND_RESP,      reject_response_command}, /* MCU -> PC，拒收。 */
    {PC_COMMAND_QUERY_TESTER_INFO, handle_query_tester_info}, /* PC -> MCU，读取FRU。 */
    {PC_COMMAND_TESTER_INFO_RESP,  reject_response_command}, /* MCU -> PC，拒收。 */
    {PC_COMMAND_START_TEST,        handle_start_test},        /* 完整校验，未接RK则拒绝执行。 */
    {PC_COMMAND_POWER_ON,          handle_power_on},          /* 非阻塞上电请求。 */
    {PC_COMMAND_POWER_OFF,         handle_power_off},         /* 等待RK安全关机。 */
    {PC_COMMAND_QUERY_POWER_STATUS, handle_query_power_status}, /* PC -> MCU，只读。 */
    {PC_COMMAND_POWER_STATUS_RESP, reject_response_command},  /* MCU -> PC，拒收。 */
    {PC_COMMAND_RESET_RK,          handle_reset_rk},          /* 等待安全关机闭环。 */
    {PC_COMMAND_SET_BOOT_MODE,     handle_set_boot_mode},     /* OFF状态写Boot DAC。 */
    {PC_COMMAND_QUERY_BOOT_MODE,   handle_query_boot_mode},   /* 查询已配置模式。 */
    {PC_COMMAND_BOOT_MODE_RESP,    reject_response_command},  /* MCU -> PC，拒收。 */
    {PC_COMMAND_SET_DDR_CONFIG, handle_set_ddr_config},       /* 选择类型/三轨拉偏。 */
    {PC_COMMAND_QUERY_POWER_MONITOR, handle_query_power_monitor}, /* 读取缓存快照。 */
    {PC_COMMAND_POWER_MONITOR_RESP, reject_response_command}, /* MCU -> PC，拒收。 */
    {PC_COMMAND_SET_RESULT_INDICATOR, handle_set_result_indicator}, /* 设置结果灯。 */
    {PC_COMMAND_QUERY_INDICATOR_STATUS, handle_query_indicator_status}, /* 查询结果灯。 */
    {PC_COMMAND_INDICATOR_STATUS_RESP, reject_response_command}, /* MCU -> PC，拒收。 */
    {PC_COMMAND_MCU_OTA_REQUEST, pc_ota_handle},              /* PC -> MCU，OTA动作表。 */
    {PC_COMMAND_MCU_OTA_RESP, reject_response_command},       /* MCU -> PC，拒收。 */
    {PC_COMMAND_MCU_OTA_COMPLETE, reject_response_command},   /* MCU -> PC，拒收。 */
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
         * 0103重复键按业务参数错误应答；共享入口已经验证UTF-8、词法、语法和
         * 对象边界，仅因重复键释放树。此错误分支重新取树，只提取唯一合法index，
         * 绝不执行命令；其他命令仍沿用原有重复键断连合同。
         */
        if (request->command_id == PC_COMMAND_START_TEST &&
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
        return PC_DISPATCH_ERROR;
    }

    /* 试运行或升级维护期间不接新测试/上电/调压；查询、心跳和下电仍可用。 */
    if (!ota_app_is_confirmed() &&
        (request->command_id == PC_COMMAND_POWER_ON ||
         request->command_id == PC_COMMAND_RESET_RK || request->command_id == PC_COMMAND_SET_BOOT_MODE ||
         request->command_id == PC_COMMAND_SET_DDR_CONFIG)) {
        cJSON_Delete(root);
        return build_command_failure(request, mcu_tx_sequence, "otaNotReady", response);
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

void pc_dispatch_poll(void)
{
#if defined(ENABLE_OTA_BOOT) && ENABLE_OTA_BOOT
    ota_download_poll();
#endif
}

void pc_dispatch_disconnected(void)
{
#if defined(ENABLE_OTA_BOOT) && ENABLE_OTA_BOOT
    ota_download_disconnected();
#endif
}
