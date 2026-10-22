#include "wire.h"
#include "crc.h"
#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)((p[1] << 8) | p[0]); }

size_t master_build(uint64_t sequence, const master_state_t *state,
                    uint8_t *out, size_t capacity)
{
    if (!state || !out || state->tester_id > 21U || state->thermal_mask > 7U) return 0U;
    size_t count = 0U;
    for (unsigned i = 0U; i < 3U; ++i) {
        if (!(state->thermal_mask & (1U << i))) continue;
        if (state->thermal_mode[i] > 1U) return 0U;
        ++count;
    }
    size_t length = 22U + (state->tester_id ? 5U : 0U) + (count ? 4U + count * 2U : 0U);
    if (capacity < length) return 0U;
    out[0] = 0x5AU; out[1] = 0U; out[2] = (uint8_t)(length - 16U);
    uint16_t crc = crc16_modbus(out, 3U);
    out[3] = (uint8_t)crc; out[4] = (uint8_t)(crc >> 8);
    for (size_t i = 13U; i > 5U;) { out[--i] = (uint8_t)sequence; sequence >>= 8; }
    size_t offset = 13U;
    const uint8_t command[] = {0x50U, 0U, 0U, 2U, 4U, 0U};
    memcpy(out + offset, command, sizeof(command)); offset += sizeof(command);
    if (state->tester_id) {
        const uint8_t field[] = {0x30U, 3U, 0U, 1U, state->tester_id};
        memcpy(out + offset, field, sizeof(field)); offset += sizeof(field);
    }
    if (count) {
        out[offset++] = 0xA0U; out[offset++] = 5U;
        out[offset++] = 0U; out[offset++] = (uint8_t)(count * 2U);
        for (unsigned i = 0U; i < 3U; ++i) {
            if (!(state->thermal_mask & (1U << i))) continue;
            out[offset++] = (uint8_t)i; out[offset++] = state->thermal_mode[i];
        }
    }
    crc = crc16_modbus(out + 5U, offset - 5U);
    out[offset++] = (uint8_t)crc; out[offset++] = (uint8_t)(crc >> 8);
    out[offset++] = 0xA5U;
    return offset;
}

size_t master_frame_size(const uint8_t header[B2_HEADER_SIZE])
{
    size_t payload = be16(header + 1U);
    return header[0] == 0x5AU && payload >= 6U && payload <= B2_MAX_PAYLOAD &&
        le16(header + 3U) == crc16_modbus(header, 3U) ? payload + 16U : 0U;
}

int master_decode(const uint8_t *frame, size_t length, uint64_t sequence, master_reply_t *reply)
{
    if (!frame || !reply || length < 27U || length > B2_MAX_FRAME ||
        master_frame_size(frame) != length || frame[length - 1U] != 0xA5U ||
        le16(frame + length - 3U) != crc16_modbus(frame + 5U, length - 8U)) return 0;
    uint64_t received = 0U;
    for (unsigned i = 5U; i < 13U; ++i) received = (received << 8) | frame[i];
    if (received != sequence) return 0;
    master_reply_t value = {0};
    unsigned seen = 0U, count = 0U;
    for (size_t offset = 13U; offset < length - 3U;) {
        size_t left = length - 3U - offset;
        if (++count > B2_MAX_TLVS || left < 4U) return 0;
        uint16_t type = be16(frame + offset);
        size_t size = be16(frame + offset + 2U);
        if (size > left - 4U) return 0;
        const uint8_t *p = frame + offset + 4U;
        unsigned bit = 0U;
        if (type == 0x5001U) {
            if (size != 2U || le16(p) != B2_COMMAND_EXCHANGE) return 0;
            bit = 1U;
        } else if (type == 0x3002U) {
            if (size != 1U || p[0] > B2_STATUS_FAILED) return 0;
            value.status = p[0]; bit = 2U;
        } else if (type == 0x3003U) {
            if (size != 1U || p[0] > 21U) return 0;
            value.tester_id = p[0]; bit = 4U;
        } else if (type == 0x3004U) {
            if (size != 1U || p[0] < 1U || p[0] > 8U) return 0;
            value.site_id = p[0]; bit = 8U;
        } else if (type == 0x5000U || type == 0xA005U) return 0;
        if (seen & bit) return 0;
        seen |= bit;
        offset += 4U + size;
    }
    if (seen != (value.status == 0U ? 15U : 3U)) return 0;
    *reply = value;
    return 1;
}
