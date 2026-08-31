#include "pc_protocol.h"
#include "cJSON.h"

#include <string.h>

static void write_be16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void write_be32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

int pc_protocol_build_response(const pc_protocol_frame_t *request,
                               uint16_t response_command_id,
                               const uint8_t *json,
                               uint32_t json_length,
                               uint8_t *output,
                               size_t output_capacity,
                               uint32_t *output_length)
{
    if (output_length) *output_length = 0U;
    if (!request || !json || json_length == 0U || !output || !output_length ||
        json_length > PC_PROTOCOL_MAX_JSON_SIZE) {
        return -1;
    }

    /* output由调用者提供，函数不在发送路径动态分配内存。 */
    const uint32_t length = PC_PROTOCOL_HEADER_SIZE + json_length;
    if (output_capacity < length) return -1;

    write_be32(&output[0], length);
    write_be16(&output[4], PC_PROTOCOL_VERSION);
    write_be16(&output[6], PC_PROTOCOL_FLAG_JSON);
    write_be16(&output[8], request->service_id);
    write_be16(&output[10], response_command_id);
    write_be32(&output[12], request->sequence); /* PC请求Seq必须原样回显。 */
    write_be16(&output[16], 0U);
    memcpy(&output[PC_PROTOCOL_HEADER_SIZE], json, json_length);
    *output_length = length;
    return 0;
}

void pc_json_configure_allocator(void *(*allocate)(size_t),
                                 void (*deallocate)(void *))
{
    /* 两个钩子必须成对提供；否则恢复cJSON默认的malloc/free。 */
    cJSON_Hooks hooks = {
        .malloc_fn = allocate,
        .free_fn = deallocate,
    };
    cJSON_InitHooks((allocate && deallocate) ? &hooks : NULL);
}

/* RFC 3629：同时拒绝过长编码、UTF-16代理区以及大于 U+10FFFF 的码点。 */
static int utf8_is_valid(const uint8_t *text, size_t length, size_t *bad_offset)
{
    size_t i = 0U;
    while (i < length) {
        const uint8_t first = text[i];
        size_t need = 0U;

        /* ASCII为单字节；非ASCII首字节决定后面必须跟1、2或3个续字节。 */
        if (first <= 0x7FU) {
            i++;
            continue;
        }
        if (first >= 0xC2U && first <= 0xDFU) {
            need = 1U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            need = 2U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            need = 3U;
        } else {
            if (bad_offset) *bad_offset = i;
            return 0;
        }

        if (i + need >= length) {
            if (bad_offset) *bad_offset = i;
            return 0;
        }
        for (size_t j = 1U; j <= need; j++) {
            if ((text[i + j] & 0xC0U) != 0x80U) {
                if (bad_offset) *bad_offset = i + j;
                return 0;
            }
        }

        /* 三/四字节序列的第二字节决定是否落入非法码点区间。 */
        const uint8_t second = text[i + 1U];
        if ((first == 0xE0U && second < 0xA0U) ||
            (first == 0xEDU && second > 0x9FU) ||
            (first == 0xF0U && second < 0x90U) ||
            (first == 0xF4U && second > 0x8FU)) {
            if (bad_offset) *bad_offset = i;
            return 0;
        }
        i += need + 1U;
    }
    return 1;
}

static int is_json_whitespace(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

/*
 * 对象通常只有少量字段，直接两两比较比额外哈希表更省代码和堆。
 * 同时递归检查数组/对象中的所有子对象，深度由 CJSON_NESTING_LIMIT 限制。
 */
static int has_duplicate_object_key(const cJSON *node)
{
    if (cJSON_IsObject(node)) {
        for (const cJSON *left = node->child; left; left = left->next) {
            for (const cJSON *right = left->next; right; right = right->next) {
                if (left->string && right->string &&
                    strcmp(left->string, right->string) == 0) {
                    return 1;
                }
            }
        }
    }

    if (cJSON_IsObject(node) || cJSON_IsArray(node)) {
        for (const cJSON *child = node->child; child; child = child->next) {
            if (has_duplicate_object_key(child)) return 1;
        }
    }
    return 0;
}

pc_json_result_t pc_json_parse_object(const uint8_t *json,
                                      size_t json_length,
                                      cJSON **root,
                                      size_t *error_offset)
{
    const char *parse_end = NULL;
    const char *body_end;
    size_t offset = 0U;

    if (root) *root = NULL;
    if (error_offset) *error_offset = 0U;
    if (!json || json_length == 0U || json_length > PC_PROTOCOL_MAX_JSON_SIZE ||
        !root) {
        return PC_JSON_ERROR_ARGUMENT;
    }

    /*
     * 先独立校验UTF-8。cJSON只负责JSON语法，不会替应用拒绝所有非法UTF-8，
     * 提前检查可保证后续保存、日志和PC显示接收到的都是合法文本。
     */
    if (!utf8_is_valid(json, json_length, &offset)) {
        if (error_offset) *error_offset = offset;
        return PC_JSON_ERROR_UTF8;
    }

    /*
     * 使用显式长度版本，因为网络正文后没有保证存在'\0'。require_null_terminated
     * 设为0，随后由本函数自行检查parse_end，允许JSON末尾存在标准空白。
     */
    cJSON *parsed = cJSON_ParseWithLengthOpts((const char *)json,
                                               json_length,
                                               &parse_end,
                                               0);
    if (!parsed) {
        if (error_offset && parse_end && parse_end >= (const char *)json) {
            *error_offset = (size_t)(parse_end - (const char *)json);
        }
        return PC_JSON_ERROR_SYNTAX;
    }

    /* 拒绝"{}xyz"等尾随数据；空格、TAB、CR、LF是JSON允许的尾随空白。 */
    body_end = (const char *)json + json_length;
    while (parse_end && parse_end < body_end && is_json_whitespace(*parse_end)) {
        parse_end++;
    }
    if (!parse_end || parse_end != body_end) {
        if (error_offset && parse_end) {
            *error_offset = (size_t)(parse_end - (const char *)json);
        }
        cJSON_Delete(parsed);
        return PC_JSON_ERROR_TRAILING_DATA;
    }
    /* PC-MCU业务包统一以对象为根，禁止数组、字符串、数字等根类型。 */
    if (!cJSON_IsObject(parsed)) {
        cJSON_Delete(parsed);
        return PC_JSON_ERROR_ROOT_TYPE;
    }
    if (has_duplicate_object_key(parsed)) {
        cJSON_Delete(parsed);
        return PC_JSON_ERROR_DUPLICATE_KEY;
    }

    *root = parsed;
    return PC_JSON_OK;
}
