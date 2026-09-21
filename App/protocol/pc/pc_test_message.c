#include "pc_test_message.h"
#include "ddr_power.h"
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
        "algorithmType", "algorithmName", "algorithmIndex", "power", "reboot", "bootMode"
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
    if (!has_exact_fields(root, fields, sizeof(fields) / sizeof(fields[0])))
        return PC_TEST_JSON_ERROR_FIELDS;

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "algorithmName");
    if (!name) return PC_TEST_JSON_ERROR_MISSING_FIELD;
    if (!cJSON_IsString(name) || !name->valuestring) {
        return PC_TEST_JSON_ERROR_FIELD_TYPE;
    }

    pc_test_json_result_t result = pc_test_start_read_index(root, &candidate.algorithm_index);
    if (result != PC_TEST_JSON_OK) return result;

    const cJSON *power = cJSON_GetObjectItemCaseSensitive(root, "power");
    const cJSON *reboot = cJSON_GetObjectItemCaseSensitive(root, "reboot");
    if (!has_exact_fields(power, power_fields, sizeof(power_fields) / sizeof(power_fields[0])))
        return PC_TEST_JSON_ERROR_FIELDS;
    if (!cJSON_IsBool(reboot)) return PC_TEST_JSON_ERROR_FIELD_TYPE;
    candidate.algorithm_name = name->valuestring;
    candidate.algorithm_type = read_name(root, "algorithmType", algorithm_types,
                                         sizeof(algorithm_types) / sizeof(algorithm_types[0]));
    candidate.memory_type = read_name(power, "memoryType", memory_types,
                                      sizeof(memory_types) / sizeof(memory_types[0]));
    candidate.boot_mode = read_name(root, "bootMode", boot_modes,
                                    sizeof(boot_modes) / sizeof(boot_modes[0]));
    if (!candidate.algorithm_type || !candidate.memory_type || !candidate.boot_mode)
        return PC_TEST_JSON_ERROR_FIELDS;
    candidate.reboot = cJSON_IsTrue(reboot) ? 1U : 0U;
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
