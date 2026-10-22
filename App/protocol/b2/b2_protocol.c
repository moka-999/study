#include "b2_protocol.h"
#include "crc.h"
#include <string.h>

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[1] << 8) | p[0]);
}

static int parse_command(b2_request_t *request, const uint8_t *value, size_t size)
{
    if (size != 2U) return 0;
    request->command = read_le16(value);
    return 1;
}

static int parse_tester(b2_request_t *request, const uint8_t *value, size_t size)
{
    if (size != 1U || value[0] < 1U || value[0] > 21U) return 0;
    request->has_tester_id = 1U;
    request->tester_id = value[0];
    return 1;
}

static int parse_modes(b2_request_t *request, const uint8_t *value, size_t size)
{
    if (size == 0U || (size & 1U) != 0U) return 0;
    for (size_t i = 0U; i < size; i += 2U) {
        uint8_t mirror = value[i];
        if (mirror >= B2_MIRROR_COUNT || value[i + 1U] > 1U ||
            (request->thermal_mask & (1U << mirror)) != 0U) return 0;
        request->thermal_mask |= (uint8_t)(1U << mirror);
        request->thermal_mode[mirror] = value[i + 1U];
    }
    return 1;
}

/* 请求不接受应答专用字段；它们不是可以静默忽略的未知扩展。 */
static int reject_response_field(b2_request_t *request, const uint8_t *value, size_t size)
{
    (void)request; (void)value; (void)size;
    return 0;
}

typedef struct {
    uint16_t type;
    int (*parse)(b2_request_t *, const uint8_t *, size_t);
} field_rule_t;

static const field_rule_t s_fields[] = {
    {0x5000U, parse_command},
    {0x3003U, parse_tester},
    {0xA005U, parse_modes},
    {0x5001U, reject_response_field},
    {0x3002U, reject_response_field},
    {0x3004U, reject_response_field},
};

int b2_request_decode(const uint8_t *frame, size_t length, b2_request_t *request)
{
    if (!frame || !request || length < 22U || length > B2_MAX_FRAME) return 0;
    size_t payload = read_be16(frame + 1U);
    if (frame[0] != 0x5AU || payload < 6U || payload > B2_MAX_PAYLOAD ||
        length != payload + 16U || frame[length - 1U] != 0xA5U ||
        read_le16(frame + 3U) != crc16_modbus(frame, 3U) ||
        read_le16(frame + length - 3U) != crc16_modbus(frame + 5U, payload + 8U)) return 0;

    memset(request, 0, sizeof(*request));
    for (size_t i = 5U; i < B2_HEADER_SIZE; ++i)
        request->sequence = (request->sequence << 8) | frame[i];
    size_t offset = B2_HEADER_SIZE;
    size_t count = 0U;
    uint32_t seen = 0U;
    unsigned int commands = 0U;
    int command_valid = 0;
    int invalid = 0;
    while (offset < B2_HEADER_SIZE + payload) {
        size_t remaining = B2_HEADER_SIZE + payload - offset;
        if (++count > B2_MAX_TLVS || remaining < 4U) return 0;
        uint16_t type = read_be16(frame + offset);
        size_t size = read_be16(frame + offset + 2U);
        if (size > remaining - 4U) return 0;
        const uint8_t *value = frame + offset + 4U;
        if (type == 0x5000U) {
            ++commands;
            command_valid = size == 2U;
        }
        for (size_t i = 0U; i < sizeof(s_fields) / sizeof(s_fields[0]); ++i) {
            if (s_fields[i].type != type) continue;
            if ((seen & (1UL << i)) != 0U || !s_fields[i].parse(request, value, size))
                invalid = 1;
            seen |= 1UL << i;
            break;
        }
        offset += 4U + size;
    }
    if (commands != 1U || !command_valid) return 0;
    request->status = invalid ? B2_STATUS_INVALID :
        request->command != B2_COMMAND_EXCHANGE ? B2_STATUS_UNSUPPORTED : B2_STATUS_OK;
    return 1;
}

static size_t append_u8(uint8_t *out, uint16_t type, uint8_t value)
{
    out[0] = (uint8_t)(type >> 8); out[1] = (uint8_t)type;
    out[2] = 0U; out[3] = 1U; out[4] = value;
    return 5U;
}

size_t b2_response_build(const b2_request_t *request, b2_status_t status,
    uint8_t tester_id, uint8_t site_id, uint8_t *out, size_t capacity)
{
    size_t length = status == B2_STATUS_OK ? 37U : 27U;
    if (!request || !out || capacity < length || (unsigned int)status > B2_STATUS_FAILED ||
        (status == B2_STATUS_OK && (tester_id > 21U || site_id < 1U || site_id > 8U))) return 0U;
    out[0] = 0x5AU; out[1] = 0U; out[2] = (uint8_t)(length - 16U);
    uint16_t crc = crc16_modbus(out, 3U);
    out[3] = (uint8_t)crc; out[4] = (uint8_t)(crc >> 8);
    uint64_t sequence = request->sequence;
    for (size_t i = 13U; i > 5U; ) { out[--i] = (uint8_t)sequence; sequence >>= 8; }
    size_t offset = 13U;
    out[offset++] = 0x50U; out[offset++] = 0x01U;
    out[offset++] = 0U; out[offset++] = 2U;
    out[offset++] = (uint8_t)request->command;
    out[offset++] = (uint8_t)(request->command >> 8);
    offset += append_u8(out + offset, 0x3002U, (uint8_t)status);
    if (status == B2_STATUS_OK) {
        offset += append_u8(out + offset, 0x3003U, tester_id);
        offset += append_u8(out + offset, 0x3004U, site_id);
    }
    crc = crc16_modbus(out + 5U, offset - 5U);
    out[offset++] = (uint8_t)crc; out[offset++] = (uint8_t)(crc >> 8);
    out[offset++] = 0xA5U;
    return offset;
}


