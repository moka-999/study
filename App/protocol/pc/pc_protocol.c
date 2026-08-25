#include "pc_protocol.h"

#include <string.h>

static uint16_t read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

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

static pc_protocol_feed_result_t fail_stream(pc_protocol_stream_t *stream,
                                             pc_protocol_error_t error)
{
    stream->error = error;
    stream->frame_ready = 0U;
    return PC_PROTOCOL_FEED_ERROR;
}

void pc_protocol_stream_reset(pc_protocol_stream_t *stream)
{
    if (!stream) return;
    stream->received = 0U;
    stream->expected_length = 0U;
    stream->error = PC_PROTOCOL_ERROR_NONE;
    stream->frame_ready = 0U;
}

pc_protocol_feed_result_t pc_protocol_stream_push_byte(
    pc_protocol_stream_t *stream, uint8_t byte)
{
    if (!stream || stream->error != PC_PROTOCOL_ERROR_NONE ||
        stream->frame_ready != 0U || stream->received >= PC_PROTOCOL_MAX_PDU) {
        if (stream) return fail_stream(stream, PC_PROTOCOL_ERROR_STATE);
        return PC_PROTOCOL_FEED_ERROR;
    }

    stream->buffer[stream->received++] = byte;

    /* Length 一到齐就拒绝过小/过大的 PDU，禁止继续吞入攻击载荷。 */
    if (stream->received == 4U) {
        stream->expected_length = read_be32(stream->buffer);
        if (stream->expected_length < PC_PROTOCOL_HEADER_SIZE ||
            stream->expected_length > PC_PROTOCOL_MAX_PDU) {
            return fail_stream(stream, PC_PROTOCOL_ERROR_LENGTH);
        }
    }

    /* Version 和 Flag 位于头部前段，可在 JSON 到达前尽早关闭非法连接。 */
    if (stream->received == 6U &&
        read_be16(&stream->buffer[4]) != PC_PROTOCOL_VERSION) {
        return fail_stream(stream, PC_PROTOCOL_ERROR_VERSION);
    }
    if (stream->received == 8U &&
        read_be16(&stream->buffer[6]) != PC_PROTOCOL_FLAG_JSON) {
        return fail_stream(stream, PC_PROTOCOL_ERROR_FLAG);
    }

    if (stream->expected_length != 0U &&
        stream->received == stream->expected_length) {
        stream->frame_ready = 1U;
        return PC_PROTOCOL_FEED_FRAME_READY;
    }
    return PC_PROTOCOL_FEED_MORE;
}

int pc_protocol_stream_get_frame(const pc_protocol_stream_t *stream,
                                 pc_protocol_frame_t *frame)
{
    if (!stream || !frame || stream->frame_ready == 0U ||
        stream->error != PC_PROTOCOL_ERROR_NONE ||
        stream->expected_length != stream->received) {
        return -1;
    }

    frame->length = read_be32(&stream->buffer[0]);
    frame->version = read_be16(&stream->buffer[4]);
    frame->flag = read_be16(&stream->buffer[6]);
    frame->service_id = read_be16(&stream->buffer[8]);
    frame->command_id = read_be16(&stream->buffer[10]);
    frame->sequence = read_be32(&stream->buffer[12]);
    frame->reserved = read_be16(&stream->buffer[16]);
    frame->json = &stream->buffer[PC_PROTOCOL_HEADER_SIZE];
    frame->json_length = frame->length - PC_PROTOCOL_HEADER_SIZE;
    return 0;
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

pc_protocol_error_t pc_protocol_stream_error(const pc_protocol_stream_t *stream)
{
    return stream ? stream->error : PC_PROTOCOL_ERROR_STATE;
}

void pc_json_configure_allocator(void *(*allocate)(size_t),
                                 void (*deallocate)(void *))
{
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

    if (!utf8_is_valid(json, json_length, &offset)) {
        if (error_offset) *error_offset = offset;
        return PC_JSON_ERROR_UTF8;
    }

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
