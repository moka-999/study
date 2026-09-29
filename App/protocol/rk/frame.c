#include "frame.h"
#include "crc.h"
#include <string.h>

/* 结果聚合数组的下标，不是线上Type/命令号；COUNT同时代表非结果字段。 */
typedef enum {
    RK_RESULT_FIELD_COMMAND,
    RK_RESULT_FIELD_ADDRESS,
    RK_RESULT_FIELD_DIE,
    RK_RESULT_FIELD_TIME,
    RK_RESULT_FIELD_COUNT
} rk_result_field_t;

typedef struct {
    uint16_t type;                 /* 完整TLV Type，用于查表。 */
    uint16_t min_value_length;     /* Value下限，字节；命令容器最少有2字节CmdID。 */
    uint16_t max_value_length;     /* Value上限，字节；标量上下限相同。 */
    rk_result_field_t result_field; /* 结果写入哪一栏；COUNT表示心跳或下行命令。 */
} rk_tlv_rule_t;

/* TLV字段规则表：编码、解码和最终结果聚合共用，避免宽度定义散落。
 * 此处只识别字段，不执行算法；真正算法由RK收到0x5000之后分发。
 */
static const rk_tlv_rule_t s_tlv_rules[] = {
    /* Type                  最小 最大             结果栏 */
    {TLV_TYPE_HEARTBEAT,      0U,  0U,              RK_RESULT_FIELD_COUNT},
    {TLV_TYPE_CMD,            2U,  TLV_VALUE_MAX,    RK_RESULT_FIELD_COUNT},
    {TLV_TYPE_RESULT_CMD,     2U,  2U,              RK_RESULT_FIELD_COMMAND},
    {TLV_TYPE_RESULT_ADDR,    8U,  8U,              RK_RESULT_FIELD_ADDRESS},
    {TLV_TYPE_RESULT_DIE,     1U,  1U,              RK_RESULT_FIELD_DIE},
    {TLV_TYPE_RESULT_TIME,    8U,  8U,              RK_RESULT_FIELD_TIME},
};

/* 返回只读规则；NULL代表未知扩展类型，由上层决定是否消费。 */
static const rk_tlv_rule_t *find_tlv_rule(uint16_t type)
{
    for (unsigned i = 0U; i < sizeof(s_tlv_rules) / sizeof(s_tlv_rules[0]); ++i) {
        if (s_tlv_rules[i].type == type) {
            return &s_tlv_rules[i];
        }
    }
    return NULL;
}

/* 已知类型查表检查宽度；未知类型仍须经过整帧边界与容量检查。 */
static int value_length_valid(uint16_t type, uint16_t length)
{
    const rk_tlv_rule_t *rule = find_tlv_rule(type);
    return rule == NULL ||
           (length >= rule->min_value_length && length <= rule->max_value_length);
}

/* 写入2字节大端值；调用者已校验输出容量，用于Length和TLV头。 */
static void write_be16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}
/* 读取小端Value；只在已知结果规则校验后调用，length为1、2或8。 */
static uint64_t read_le(const uint8_t *value, uint16_t length)
{
    uint64_t result = 0U;
    for (uint16_t i = 0U; i < length; ++i) result |= (uint64_t)value[i] << (8U * i);
    return result;
}
void frame_init(tlv_frame_t *frame)
{
    if (frame != NULL) memset(frame, 0, sizeof(*frame));
}
/* 返回线上TLV总长度，非法帧返回0；32位累加避免长度回绕。 */
static uint16_t payload_length(const tlv_frame_t *frame)
{
    if (frame == NULL || frame->count == 0U || frame->count > TLV_MAX_PER_FRAME ||
        frame->payload_used > TLV_PAYLOAD_MAX) return 0U;
    uint32_t length = 0U;
    for (uint8_t i = 0U; i < frame->count; ++i) {
        const tlv_unit_t *unit = &frame->units[i];
        if (unit->data_off > frame->payload_used ||
            unit->data_len > frame->payload_used - unit->data_off ||
            !value_length_valid(unit->type, unit->data_len)) return 0U;
        length += 4U + unit->data_len;
        if (length > TLV_PAYLOAD_MAX) return 0U;
    }
    return (uint16_t)length;
}
int tlv_pack(tlv_frame_t *frame, uint16_t type, const uint8_t *value, uint16_t value_len)
{
    if (frame == NULL || frame->count >= TLV_MAX_PER_FRAME ||
        frame->payload_used > TLV_PAYLOAD_MAX ||
        (value == NULL && value_len != 0U) || !value_length_valid(type, value_len)) return -1;
    uint16_t used = frame->count == 0U ? 0U : payload_length(frame);
    if ((frame->count != 0U && used == 0U) ||
        (uint32_t)used + 4U + value_len > TLV_PAYLOAD_MAX ||
        value_len > TLV_PAYLOAD_MAX - frame->payload_used) return -1;
    tlv_unit_t *unit = &frame->units[frame->count];
    unit->type = type;
    unit->data_off = frame->payload_used;
    unit->data_len = value_len;
    if (value_len != 0U) memcpy(frame->payload + frame->payload_used, value, value_len);
    frame->payload_used += value_len;
    ++frame->count;
    return 0;
}
int tlv_pack_frame(const tlv_frame_t *in, uint64_t seq, uint8_t *out, uint16_t out_max)
{
    uint16_t length = payload_length(in);
    if (out == NULL || length == 0U || out_max < length + 16U) return 0;
    out[0] = TLV_SYNC_HEADER;
    write_be16(out + 1, length);
    uint16_t crc = crc16_modbus(out, 3U);
    out[3] = (uint8_t)crc;
    out[4] = (uint8_t)(crc >> 8);
    for (uint8_t i = 0U; i < 8U; ++i) out[5U + i] = (uint8_t)(seq >> (56U - 8U * i));
    uint16_t pos = 13U;
    for (uint8_t i = 0U; i < in->count; ++i) {
        const tlv_unit_t *unit = &in->units[i];
        write_be16(out + pos, unit->type);
        write_be16(out + pos + 2U, unit->data_len);
        if (unit->data_len != 0U) memcpy(out + pos + 4U, tlv_unit_get_data(in, unit), unit->data_len);
        pos += 4U + unit->data_len;
    }
    crc = crc16_modbus(out + 5, length + 8U);
    out[pos++] = (uint8_t)crc;
    out[pos++] = (uint8_t)(crc >> 8);
    out[pos++] = TLV_SYNC_FOOTER;
    return pos;
}
int tlv_pack_cmd(uint16_t cmd, const uint8_t *data, uint16_t data_len,
                 uint64_t seq, uint8_t *out, uint16_t out_max)
{
    if (out == NULL || data_len > TLV_CMD_DATA_MAX || out_max < TLV_CMD_FRAME_SIZE + data_len ||
        (data_len != 0U && data == NULL)) return 0;
    tlv_frame_t frame;
    frame_init(&frame);
    frame.count = 1U;
    frame.payload_used = data_len + 2U;
    frame.units[0].type = TLV_TYPE_CMD;
    frame.units[0].data_len = data_len + 2U;
    frame.payload[0] = (uint8_t)cmd;
    frame.payload[1] = (uint8_t)(cmd >> 8);
    if (data_len != 0U) memcpy(frame.payload + 2, data, data_len);
    return tlv_pack_frame(&frame, seq, out, out_max);
}
int tlv_pack_heartbeat(uint64_t seq, uint8_t *out, uint16_t out_max)
{
    tlv_frame_t frame;
    frame_init(&frame);
    (void)tlv_pack(&frame, TLV_TYPE_HEARTBEAT, NULL, 0U);
    return tlv_pack_frame(&frame, seq, out, out_max);
}
int tlv_unpack(const uint8_t *wire, uint16_t wire_len, tlv_frame_t *out)
{
    if (out == NULL) return -1;
    frame_init(out);
    if (wire == NULL || wire_len < TLV_PAYLOAD_MIN + 16U || wire_len > TLV_FRAME_MAX) return -1;
    uint16_t length = tlv_get_length(wire + 1);
    if (length < TLV_PAYLOAD_MIN || length > TLV_PAYLOAD_MAX || wire_len != length + 16U ||
        wire[0] != TLV_SYNC_HEADER || wire[wire_len - 1U] != TLV_SYNC_FOOTER ||
        crc16_modbus(wire, 5U) != 0U || crc16_modbus(wire + 5, length + 10U) != 0U) return -1;
    for (uint8_t i = 0U; i < 8U; ++i) out->sequence = (out->sequence << 8) | wire[5U + i];
    uint16_t offset = 13U, end = 13U + length;
    while (offset < end) {
        if ((uint16_t)(end - offset) < 4U) goto invalid_frame;
        uint16_t type = tlv_get_length(wire + offset);
        uint16_t size = tlv_get_length(wire + offset + 2U);
        offset += 4U;
        if (size > end - offset || tlv_pack(out, type, wire + offset, size) != 0) goto invalid_frame;
        offset += size;
    }
    return out->count;
invalid_frame:
    frame_init(out);
    return -1;
}
int rk_result_decode(const tlv_frame_t *frame, rk_test_result_t *result)
{
    if (result == NULL) return -1;
    memset(result, 0, sizeof(*result));
    if (payload_length(frame) == 0U) return -1;
    /* 先在临时数组汇总，四个字段全部合法后才写result，失败保持全零输出。 */
    uint64_t values[RK_RESULT_FIELD_COUNT] = {0U};
    uint8_t seen = 0U;
    for (uint8_t i = 0U; i < frame->count; ++i) {
        const tlv_unit_t *unit = &frame->units[i];
        const rk_tlv_rule_t *rule = find_tlv_rule(unit->type);
        if (unit->type == TLV_TYPE_CMD) return -1;
        if (rule == NULL || rule->result_field == RK_RESULT_FIELD_COUNT) {
            continue; /* 未知扩展和空心跳不补齐任何必需结果字段。 */
        }
        uint8_t field_bit = (uint8_t)(1U << rule->result_field);
        if ((seen & field_bit) != 0U) return -1;
        seen |= field_bit;
        values[rule->result_field] = read_le(tlv_unit_get_data(frame, unit), unit->data_len);
    }
    /* 四栏分别占bit0..3，全部到齐为0b1111；不使用地址默认0来冒充PASS。 */
    if (seen != (1U << RK_RESULT_FIELD_COUNT) - 1U) return -1;
    result->command_id = (uint16_t)values[RK_RESULT_FIELD_COMMAND];
    result->error_address = values[RK_RESULT_FIELD_ADDRESS];
    result->die = (uint8_t)values[RK_RESULT_FIELD_DIE];
    result->elapsed_ms = values[RK_RESULT_FIELD_TIME];
    return 0;
}
