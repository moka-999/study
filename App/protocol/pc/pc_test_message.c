#include "pc_test_message.h"
#include "ddr_power.h"
#include "idd_stats.h"
#include <math.h>
#include <string.h>

static pc_test_json_result_t read_required_u32(const cJSON *root,
                                               const char *name,
                                               uint32_t *value)
{
    /* CaseSensitive保证algorithmIndex等协议字段大小写错误时不会被悄悄接受。 */
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item) return PC_TEST_JSON_ERROR_MISSING_FIELD;
    if (!cJSON_IsNumber(item)) return PC_TEST_JSON_ERROR_FIELD_TYPE;

    /*
     * cJSON把所有JSON number保存为double；范围合法还不够，必须再做一次
     * 往返比较，拒绝1.5这类不能无损转换为uint32_t的值。
     */
    const double number = item->valuedouble;
    if (!isfinite(number) || number < 0.0 || number > 4294967295.0) {
        return PC_TEST_JSON_ERROR_INTEGER_RANGE;
    }
    const uint32_t integer = (uint32_t)number;
    if ((double)integer != number) return PC_TEST_JSON_ERROR_FIELD_TYPE;
    *value = integer;
    return PC_TEST_JSON_OK;
}

pc_test_json_result_t pc_test_start_read_index(const cJSON *root, uint32_t *index)
{
    size_t count = 0U;
    if (!root || !index || !cJSON_IsObject(root))
        return PC_TEST_JSON_ERROR_ARGUMENT;
    for (const cJSON *item = root->child; item; item = item->next) {
        if (item->string && strcmp(item->string, "algorithmIndex") == 0) count++;
    }
    if (count == 0U) return PC_TEST_JSON_ERROR_MISSING_FIELD;
    if (count != 1U) return PC_TEST_JSON_ERROR_FIELDS;
    return read_required_u32(root, "algorithmIndex", index);
}

/* 总数相同且每个必填键都出现一次，才允许继续取值；重复键不能隐式取第一项。 */
static int has_exact_fields(const cJSON *root, const char *const *fields, size_t count)
{
    size_t actual = 0U;
    if (!cJSON_IsObject(root)) return 0;
    for (const cJSON *item = root->child; item; item = item->next) actual++;
    if (actual != count) return 0;
    for (size_t i = 0U; i < count; i++) {
        if (!cJSON_GetObjectItemCaseSensitive(root, fields[i])) return 0;
    }
    return 1;
}

static const char *read_name(const cJSON *root, const char *field,
                            const char *const *names, size_t count)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!cJSON_IsString(item) || !item->valuestring) return NULL;
    for (size_t i = 0U; i < count; i++) {
        if (strcmp(item->valuestring, names[i]) == 0) return item->valuestring;
    }
    return NULL;
}

pc_test_json_result_t pc_test_start_parse(const cJSON *root,
                                          pc_test_start_view_t *view)
{
    static const char *const fields[] = {
        "algorithmType", "algorithmName", "algorithmIndex", "power", "reboot", "bootMode",
        "timeoutMs"
    };
    static const char *const power_fields[] = {
        "memoryType", "vdd1Steps", "vdd2Steps", "vddqSteps"
    };
    static const char *const algorithm_types[] = {"functional", "slt", "parametric"};
    static const char *const memory_types[] = {"LPDDR4", "LPDDR4X"};
    static const char *const boot_modes[] = {
        "usbMaskrom", "sdCard", "emmc", "fspiM0", "fspiM1", "fspiM2", "emmcSd"
    };
    pc_test_start_view_t candidate = {0};
    if (!root || !view || !cJSON_IsObject(root)) {
        return PC_TEST_JSON_ERROR_ARGUMENT;
    }
    const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(root, "timeoutMs");
    const size_t field_count = sizeof(fields) / sizeof(fields[0]) - (timeout ? 0U : 1U);
    if (!has_exact_fields(root, fields, field_count))
        return PC_TEST_JSON_ERROR_FIELDS;

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "algorithmName");
    if (!name) return PC_TEST_JSON_ERROR_MISSING_FIELD;
    if (!cJSON_IsString(name) || !name->valuestring) {
        return PC_TEST_JSON_ERROR_FIELD_TYPE;
    }

    pc_test_json_result_t result = pc_test_start_read_index(root, &candidate.algorithm_index);
    if (result != PC_TEST_JSON_OK) return result;
    if (timeout) {
        result = read_required_u32(root, "timeoutMs", &candidate.timeout_ms);
        if (result != PC_TEST_JSON_OK) return result;
        if (candidate.timeout_ms > 86400000U) return PC_TEST_JSON_ERROR_INTEGER_RANGE;
    }

    const cJSON *power = cJSON_GetObjectItemCaseSensitive(root, "power");
    const cJSON *reboot = cJSON_GetObjectItemCaseSensitive(root, "reboot");
    const cJSON *boot = cJSON_GetObjectItemCaseSensitive(root, "bootMode");
    if (!cJSON_IsBool(reboot)) return PC_TEST_JSON_ERROR_FIELD_TYPE;
    candidate.algorithm_name = name->valuestring;
    candidate.algorithm_type = read_name(root, "algorithmType", algorithm_types,
                                         sizeof(algorithm_types) / sizeof(algorithm_types[0]));
    if (!candidate.algorithm_type) return PC_TEST_JSON_ERROR_FIELDS;
    candidate.reboot = cJSON_IsTrue(reboot) ? 1U : 0U;
    if (cJSON_IsNull(power) || cJSON_IsNull(boot)) {
        /* null明确表示不接管电源/启动介质，不能与单独调压或重启请求混用。 */
        if (!cJSON_IsNull(power) || !cJSON_IsNull(boot) || candidate.reboot)
            return PC_TEST_JSON_ERROR_FIELDS;
        candidate.external_ready = 1U;
        *view = candidate;
        return PC_TEST_JSON_OK;
    }
    if (!has_exact_fields(power, power_fields, sizeof(power_fields) / sizeof(power_fields[0])))
        return PC_TEST_JSON_ERROR_FIELDS;
    candidate.memory_type = read_name(power, "memoryType", memory_types,
                                      sizeof(memory_types) / sizeof(memory_types[0]));
    candidate.boot_mode = read_name(root, "bootMode", boot_modes,
                                    sizeof(boot_modes) / sizeof(boot_modes[0]));
    if (!candidate.memory_type || !candidate.boot_mode)
        return PC_TEST_JSON_ERROR_FIELDS;
    for (size_t rail = 0U; rail < DDR_RAIL_COUNT; rail++) {
        const cJSON *step = cJSON_GetObjectItemCaseSensitive(power, power_fields[rail + 1U]);
        if (!cJSON_IsNumber(step)) return PC_TEST_JSON_ERROR_FIELD_TYPE;
        double number = step->valuedouble;
        if (!isfinite(number) || number < INT16_MIN || number > INT16_MAX)
            return PC_TEST_JSON_ERROR_INTEGER_RANGE;
        candidate.power_steps[rail] = (int16_t)number;
        if ((double)candidate.power_steps[rail] != number)
            return PC_TEST_JSON_ERROR_FIELD_TYPE;
    }
    /* 复用纯计算入口，只判定TPS能否编码；安全范围、RK就绪和执行权限仍未放行。 */
    ddr_memory_type_t type = strcmp(candidate.memory_type, "LPDDR4") == 0
                                ? DDR_MEMORY_LPDDR4 : DDR_MEMORY_LPDDR4X;
    if (ddr_power_build_step_targets(type, candidate.power_steps, candidate.target_mv) != IF_OK)
        return PC_TEST_JSON_ERROR_POWER_RANGE;
    *view = candidate;
    return PC_TEST_JSON_OK;
}

cJSON *pc_test_result_create(const char *algorithm_name,
                             uint32_t algorithm_index,
                             const pc_chip_test_result_t *results,
                             size_t result_count)
{
    if (!algorithm_name || (result_count != 0U && !results)) return NULL;

    /*
     * cJSON父节点拥有所有已挂接子节点。任一步失败时只需删除root，就会递归
     * 释放algorithmName、algorithmIndex、testResults及已加入的结果项。
     */
    cJSON *root = cJSON_CreateObject();
    cJSON *array = NULL;
    if (!root || !cJSON_AddStringToObject(root, "algorithmName", algorithm_name) ||
        !cJSON_AddNumberToObject(root, "algorithmIndex", algorithm_index)) {
        cJSON_Delete(root);
        return NULL;
    }

    array = cJSON_AddArrayToObject(root, "testResults");
    if (!array) {
        cJSON_Delete(root);
        return NULL;
    }

    for (size_t i = 0U; i < result_count; i++) {
        if (!results[i].site_id) {
            cJSON_Delete(root);
            return NULL;
        }
        /* entry成功加入array后所有权转交给root；加入前失败则单独删除entry。 */
        cJSON *entry = cJSON_CreateObject();
        if (!entry ||
            !cJSON_AddStringToObject(entry, "siteId", results[i].site_id) ||
            !cJSON_AddNumberToObject(entry, "mirrorIndex",
                                    results[i].mirror_index) ||
            !cJSON_AddStringToObject(entry, "resultDesc",
                                    results[i].result_desc ? results[i].result_desc : "") ||
            !cJSON_AddNumberToObject(entry, "testStatus",
                                    results[i].test_status)) {
            cJSON_Delete(entry);
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToArray(array, entry);
    }
    return root;
}

cJSON *pc_functional_result_create(const char *algorithm_name,
                                    uint32_t algorithm_index,
                                    const char *site_id,
                                    uint32_t mirror_index,
                                    const char *physical_addr,
                                    uint64_t error_address,
                                    uint64_t elapsed_ms)
{
    if (!algorithm_name || !site_id || !physical_addr) return NULL;
    /* uint64最多20位十进制数；只由整数生成Raw数字，避免double丢低位，禁止外部文本进入。 */
    char elapsed_text[21];
    char *elapsed_digits = elapsed_text + sizeof(elapsed_text) - 1U;
    *elapsed_digits = '\0';
    do {
        *--elapsed_digits = (char)('0' + elapsed_ms % 10U);
        elapsed_ms /= 10U;
    } while (elapsed_ms != 0U);
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddStringToObject(root, "algorithmType", "functional") ||
        !cJSON_AddStringToObject(root, "algorithmName", algorithm_name) ||
        !cJSON_AddNumberToObject(root, "algorithmIndex", algorithm_index) ||
        !cJSON_AddRawToObject(root, "testTimeMs", elapsed_digits)) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *array = cJSON_AddArrayToObject(root, "testResults");
    cJSON *entry = cJSON_CreateObject();
    if (!array || !entry || !cJSON_AddItemToArray(array, entry)) {
        cJSON_Delete(entry);
        cJSON_Delete(root);
        return NULL;
    }
    /* entry已交给root管理；后续任一分配失败均删除root，避免半包和内存泄漏。 */
    if (!cJSON_AddStringToObject(entry, "siteId", site_id) ||
        !cJSON_AddNumberToObject(entry, "mirrorIndex", mirror_index) ||
        !cJSON_AddNumberToObject(entry, "testStatus", error_address == 0U ? 1 : 2)) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *desc = cJSON_AddObjectToObject(entry, "resultDesc");
    /* 按十六进制逐位输出u64，避免浮点JSON数字丢精度及精简printf不支持%llX。 */
    static const char digits[] = "0123456789ABCDEF";
    char address[] = "0x0000000000000000";
    for (size_t i = 0U; i < 16U; i++) {
        address[2U + i] = digits[(error_address >> ((15U - i) * 4U)) & 0xFU];
    }
    if (!desc ||
        !(error_address == 0U ? cJSON_AddNullToObject(desc, "firstErrorAddress")
                             : cJSON_AddStringToObject(desc, "firstErrorAddress", address)) ||
        !cJSON_AddNullToObject(desc, "errorType") ||
        !cJSON_AddStringToObject(entry, "physicalAddr", physical_addr) ||
        !cJSON_AddNullToObject(entry, "measurementData")) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

/* mean/min/max均在int32/uint32采样值范围内，转换成JSON double不会丢整数精度。 */
static cJSON *metric_value(const idd_metric_stats_t *metric, unsigned kind)
{
    int64_t value = kind == 0U ? metric->mean : kind == 1U ? metric->maximum : metric->minimum;
    return metric->valid_count ? cJSON_CreateNumber((double)value) : cJSON_CreateNull();
}

int pc_test_add_statistics(cJSON *body, const idd_stats_t *snapshot)
{
    static const char *const states[] = {"idle", "running", "complete", "timeout", "cancelled"};
    static const char *const names[] = {"voltageAvgMv", "voltagePeakMv", "voltageMinMv",
                                      "currentAvgMa", "currentPeakMa", "currentMinMa"};
    if (!cJSON_IsObject(body) || !snapshot ||
        (unsigned)snapshot->state >= sizeof(states) / sizeof(states[0])) return 0;
    if (snapshot->state == IDD_STATS_IDLE) return cJSON_AddNullToObject(body, "statistics") != NULL;
    cJSON *stats = cJSON_AddObjectToObject(body, "statistics");
    if (!stats || !cJSON_AddStringToObject(stats, "state", states[snapshot->state])) return 0;
    for (unsigned field = 0; field < 6U; ++field) {
        cJSON *array = cJSON_AddArrayToObject(stats, names[field]);
        if (!array) return 0;
        for (unsigned rail = 0; rail < MEMORY_MONITOR_RAIL_COUNT; ++rail) {
            cJSON *number = metric_value(field < 3U ? &snapshot->voltage[rail] : &snapshot->current[rail], field % 3U);
            if (!number || !cJSON_AddItemToArray(array, number)) { cJSON_Delete(number); return 0; }
        }
    }
    return 1;
}

int pc_test_add_measurement(cJSON *body, const idd_stats_t *snapshot)
{
    static const char *const names[] = {
        "Vdd1Ave", "Vdd1Peak", "Vdd1Min", "Vdd2Ave", "Vdd2Peak", "Vdd2Min",
        "VddqAve", "VddqPeak", "VddqMin", "Idd1Ave", "Idd1Peak", "Idd1Min",
        "Idd2Ave", "Idd2Peak", "Idd2Min", "IddqAve", "IddqPeak", "IddqMin"
    };
    cJSON *item = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(body, "testResults"), 0);
    if (!cJSON_IsObject(item) || !snapshot) return 0;
    cJSON *measurement = cJSON_CreateObject();
    if (!measurement) return 0;
    for (unsigned i = 0; i < 18U; ++i) {
        const idd_metric_stats_t *metric = i < 9U ? &snapshot->voltage[i / 3U] : &snapshot->current[(i - 9U) / 3U];
        cJSON *number = metric_value(metric, i % 3U);
        if (!number || !cJSON_AddItemToObject(measurement, names[i], number)) {
            cJSON_Delete(number); cJSON_Delete(measurement); return 0;
        }
    }
    if (!cJSON_ReplaceItemInObjectCaseSensitive(item, "measurementData", measurement)) {
        cJSON_Delete(measurement); return 0;
    }
    return 1;
}
