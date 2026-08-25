#include "pc_test_message.h"

static pc_test_json_result_t read_required_u32(const cJSON *root,
                                               const char *name,
                                               uint32_t *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item) return PC_TEST_JSON_ERROR_MISSING_FIELD;
    if (!cJSON_IsNumber(item)) return PC_TEST_JSON_ERROR_FIELD_TYPE;

    const double number = item->valuedouble;
    if (number < 0.0 || number > 4294967295.0) {
        return PC_TEST_JSON_ERROR_INTEGER_RANGE;
    }
    const uint32_t integer = (uint32_t)number;
    if ((double)integer != number) return PC_TEST_JSON_ERROR_FIELD_TYPE;
    *value = integer;
    return PC_TEST_JSON_OK;
}

pc_test_json_result_t pc_test_start_parse(const cJSON *root,
                                          pc_test_start_view_t *view)
{
    if (!root || !view || !cJSON_IsObject(root)) {
        return PC_TEST_JSON_ERROR_ARGUMENT;
    }

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "algorithmName");
    if (!name) return PC_TEST_JSON_ERROR_MISSING_FIELD;
    if (!cJSON_IsString(name) || !name->valuestring) {
        return PC_TEST_JSON_ERROR_FIELD_TYPE;
    }

    uint32_t algorithm_index = 0U;
    pc_test_json_result_t result = read_required_u32(
        root, "algorithmIndex", &algorithm_index);
    if (result != PC_TEST_JSON_OK) return result;

    view->algorithm_name = name->valuestring;
    view->algorithm_index = algorithm_index;
    return PC_TEST_JSON_OK;
}

cJSON *pc_test_result_create(const char *algorithm_name,
                             uint32_t algorithm_index,
                             const pc_chip_test_result_t *results,
                             size_t result_count)
{
    if (!algorithm_name || (result_count != 0U && !results)) return NULL;

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
        cJSON *entry = cJSON_CreateObject();
        if (!entry ||
            !cJSON_AddStringToObject(entry, "siteId", results[i].site_id) ||
            !cJSON_AddNumberToObject(entry, "mirrorIndex",
                                    results[i].mirror_index) ||
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
