#include "frame.h"
#include "crc.h"
#include <string.h>

/* 结果聚合数组的下标，不是线上Type/命令号；COUNT同时代表非结果字段。 */
typedef enum {
    RK_RESULT_FIELD_COMMAND,
    RK_RESULT_FIELD_ADDRESS,
    RK_RESULT_FIELD_DIE,
    RK_RESULT_FIELD_TIME,
    RK_RESULT_FIELD_PROGRESS,
    RK_RESULT_FIELD_FAIL,
    RK_RESULT_FIELD_COUNT
} rk_result_field_t;

typedef struct {
    uint16_t type;                  /* 完整TLV Type，用于查表。 */
    uint16_t min_value_length;      /* Value下限，字节；命令容器最少有2字节CmdID。 */
    uint16_t max_value_length;      /* Value上限，字节；标量上下限相同。 */
    rk_result_field_t result_field; /* 结果写入哪一栏；COUNT表示心跳或下行命令。 */
    uint8_t max_u8;                 /* 1字节标量的上限；0表示本规则不检查数值。 */
} rk_tlv_rule_t;

/* TLV字段规则表：编码、解码和最终结果聚合共用，避免宽度定义散落。
 * 此处只识别字段，不执行算法；真正算法由RK收到0x5000之后分发。
 */
static const rk_tlv_rule_t s_tlv_rules[] = {
    /* Type                      最小  最大              结果栏                  单字节上限 */
    {TLV_TYPE_HEARTBEAT,      0U,  0U,              RK_RESULT_FIELD_COUNT,          0U},
    {TLV_TYPE_CMD,            2U,  TLV_VALUE_MAX,   RK_RESULT_FIELD_COUNT,          0U},
    {TLV_TYPE_RESULT_CMD,     2U,  2U,              RK_RESULT_FIELD_COMMAND,        0U},
    {TLV_TYPE_RESULT_ADDR,    8U,  8U,              RK_RESULT_FIELD_ADDRESS,        0U},
    {TLV_TYPE_RESULT_DIE,     1U,  1U,              RK_RESULT_FIELD_DIE,          255U},
    {TLV_TYPE_RESULT_TIME,    8U,  8U,              RK_RESULT_FIELD_TIME,           0U},
    {TLV_TYPE_PROGRESS,       1U,  1U,              RK_RESULT_FIELD_PROGRESS,     100U},
    {TLV_TYPE_RESULT_FAIL,    1U,  1U,              RK_RESULT_FIELD_FAIL,           1U},
    {TLV_TYPE_REGISTER_ADDRESS, 4U, 4U,            RK_RESULT_FIELD_COUNT,          0U},
    {TLV_TYPE_PARAM_ACTION,   1U,  1U,              RK_RESULT_FIELD_COUNT,          1U},
    {TLV_TYPE_TIMING_VALUE,   4U,  4U,              RK_RESULT_FIELD_COUNT,          0U},
    {TLV_TYPE_FREQUENCY_VALUE, 8U, 8U,              RK_RESULT_FIELD_COUNT,          0U},
};

/* 返回只读规则；NULL代表未知扩展类型，由上层决定是否消费。 */
static const rk_tlv_rule_t *find_tlv_rule(uint16_t type)
{
    for (unsigned i = 0U; i < sizeof(s_tlv_rules) / sizeof(s_tlv_rules[0]); ++i) {
        // 遍历表，查找匹配的Type
        if (s_tlv_rules[i].type == type) {
            return &s_tlv_rules[i];
        }
    }
    return NULL;
}

/* 已知类型查表检查宽度和标量范围；未知类型仍须通过边界及容量检查。 */
static int value_valid(uint16_t type, const uint8_t *value, uint16_t length)
{
    //检查value是否合法
    if (length != 0U && value == NULL) return 0;
    const rk_tlv_rule_t *rule = find_tlv_rule(type);
    return rule == NULL ||
           (length >= rule->min_value_length && length <= rule->max_value_length &&
            (rule->max_u8 == 0U || (length == 1U && value[0] <= rule->max_u8)));
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
//初始化帧结构体
void frame_init(tlv_frame_t *frame)
{
    if (frame != NULL) memset(frame, 0, sizeof(*frame));
}
/* 返回线上TLV总长度，0是合法空心跳，-1才是非法帧；32位累加避免回绕。 */
// 计算帧中所有TLV的总长度
static int payload_length(const tlv_frame_t *frame)
{
    if (frame == NULL || frame->count > TLV_MAX_PER_FRAME ||
        frame->payload_used > TLV_PAYLOAD_MAX ||
        (frame->count == 0U && frame->payload_used != 0U)) return -1;
    uint32_t length = 0U;
    for (uint8_t i = 0U; i < frame->count; ++i) {
        const tlv_unit_t *unit = &frame->units[i];
        if (unit->data_off > frame->payload_used ||
            unit->data_len > frame->payload_used - unit->data_off ||
            !value_valid(unit->type, tlv_unit_get_data(frame, unit), unit->data_len)) return -1;
        length += 4U + unit->data_len;
        if (length > TLV_PAYLOAD_MAX) return -1;
    }
    return (int)length;
}
//向frame中追加一个TLV单元
int tlv_pack(tlv_frame_t *frame, uint16_t type, const uint8_t *value, uint16_t value_len)
{
    if (frame == NULL || frame->count >= TLV_MAX_PER_FRAME ||
        frame->payload_used > TLV_PAYLOAD_MAX ||
        !value_valid(type, value, value_len)) return -1;
    int used = payload_length(frame);
    if (used < 0 ||
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
// 将TLV组装为完整帧，包含头、长度、CRC和尾
int tlv_pack_frame(const tlv_frame_t *in, uint64_t seq, uint8_t *out, uint16_t out_max)
{
    int length = payload_length(in);
    if (out == NULL || length < 0 || out_max < (unsigned)length + 16U) return 0;

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
// 组装算法命令帧的便捷封装；只组帧，不发送串口数据。
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

/* 进入/退出各是一条独立请求；本函数只编码已确认的字段，不推断RK的阶段应答。 */
int tlv_pack_idd_action(uint16_t cmd, uint8_t action, uint64_t seq,
                        uint8_t *out, uint16_t out_max)
{
    if (cmd < 21U || cmd > 36U || action > 1U ||
        out == NULL || out_max < TLV_IDD_ACTION_FRAME_SIZE) return 0;
    tlv_frame_t frame;
    frame_init(&frame);
    uint8_t command[2] = {(uint8_t)cmd, (uint8_t)(cmd >> 8)};
    if (tlv_pack(&frame, TLV_TYPE_CMD, command, sizeof(command)) != 0 ||
        tlv_pack(&frame, TLV_TYPE_PARAM_ACTION, &action, sizeof(action)) != 0) return 0;
    return tlv_pack_frame(&frame, seq, out, out_max);
}

int rk_is_timing_command(uint16_t cmd)
{
    return cmd >= 0x25U && cmd <= 0x34U && cmd != 0x27U;
}

int tlv_pack_timing(uint16_t cmd, uint32_t value, uint64_t seq, uint8_t *out, uint16_t out_max)
{
    if (!rk_is_timing_command(cmd) || !out || out_max < TLV_TIMING_FRAME_SIZE) return 0;
    tlv_frame_t frame;
    frame_init(&frame);
    uint8_t command[2] = {(uint8_t)cmd, (uint8_t)(cmd >> 8)};
    uint8_t bytes[4];
    for (unsigned i = 0U; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)(value >> (8U * i));
    if (tlv_pack(&frame, TLV_TYPE_CMD, command, sizeof(command)) != 0 ||
        tlv_pack(&frame, TLV_TYPE_TIMING_VALUE, bytes, sizeof(bytes)) != 0) return 0;
    return tlv_pack_frame(&frame, seq, out, out_max);
}

int tlv_pack_frequency(int64_t value, uint64_t seq, uint8_t *out, uint16_t out_max)
{
    if (!out || out_max < TLV_FREQUENCY_FRAME_SIZE) return 0;
    tlv_frame_t frame;
    frame_init(&frame);
    uint8_t command[2] = {RK_CMD_QUERY_FREQUENCY, 0U};
    uint8_t bytes[8];
    for (unsigned i = 0U; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)((uint64_t)value >> (8U * i));
    if (tlv_pack(&frame, TLV_TYPE_RESULT_CMD, command, sizeof(command)) != 0 ||
        tlv_pack(&frame, TLV_TYPE_FREQUENCY_VALUE, bytes, sizeof(bytes)) != 0) return 0;
    return tlv_pack_frame(&frame, seq, out, out_max);
}

int rk_is_frequency_query(const tlv_frame_t *frame)
{
    if (!frame || frame->count != 1U || frame->units[0].type != TLV_TYPE_CMD ||
        frame->units[0].data_len != 2U || frame->units[0].data_off > TLV_PAYLOAD_MAX - 2U ||
        frame->payload_used < frame->units[0].data_off + 2U) return 0;
    return read_le(frame->payload + frame->units[0].data_off, 2U) == RK_CMD_QUERY_FREQUENCY;
}

int tlv_pack_heartbeat(uint64_t seq, uint8_t *out, uint16_t out_max)
{
    tlv_frame_t frame;
    frame_init(&frame);
    return tlv_pack_frame(&frame, seq, out, out_max);
}

int tlv_unpack(const uint8_t *wire, uint16_t wire_len, tlv_frame_t *out)
{
    // 检查输入参数和帧头尾、长度、CRC
    if (out == NULL) return -1;
    frame_init(out);
    if (wire == NULL || wire_len < TLV_FRAME_MIN || wire_len > TLV_FRAME_MAX) return -1;
    uint16_t length = tlv_get_length(wire + 1);
    if ((length != 0U && length < TLV_PAYLOAD_MIN) || length > TLV_PAYLOAD_MAX || wire_len != length + 16U ||
        wire[0] != TLV_SYNC_HEADER || wire[wire_len - 1U] != TLV_SYNC_FOOTER ||
        crc16_modbus(wire, 5U) != 0U || crc16_modbus(wire + 5, length + 10U) != 0U) return -1;
    // 解析seq
    for (uint8_t i = 0U; i < 8U; ++i) out->sequence = (out->sequence << 8) | wire[5U + i];
    uint16_t offset = 13U, end = 13U + length;
    // 遍历解析TLV单元
    while (offset < end) {
        if ((uint16_t)(end - offset) < 4U) goto invalid_frame;
        uint16_t type = tlv_get_length(wire + offset);
        uint16_t size = tlv_get_length(wire + offset + 2U);
        offset += 4U;
        //把解析出来的TLV单元追加到frameout中，如果失败则返回-1
        if (size > end - offset || tlv_pack(out, type, wire + offset, size) != 0) goto invalid_frame;
        offset += size;
    }
    return out->count;
invalid_frame:
    frame_init(out);
    return -1;
}

int rk_final_result_decode(const tlv_frame_t *frame, rk_final_result_t *result)
{
    if (result == NULL) return -1;
    memset(result, 0, sizeof(*result));
    // 检查帧长度和TLV数量，必须是3、5或7个TLV
    if (payload_length(frame) < 0 ||
        (frame->count != 3U && frame->count != 5U && frame->count != 7U)) return -1;
    //记载解码结果的临时结构体，初始化为0
    rk_final_result_t decoded = {0};
    uint64_t values[RK_RESULT_FIELD_COUNT] = {0U};
    uint64_t pending_address = 0U;
    uint8_t seen = 0U, seen_dies = 0U, awaiting_die = 0U;
    // 遍历每个TLV单元，检查类型和长度，并将结果写入临时结构体
    for (uint8_t i = 0U; i < frame->count; ++i) {
        // 获取TLV单元和规则
        const tlv_unit_t *unit = &frame->units[i];//获取第i个tlv的描述
        const rk_tlv_rule_t *rule = find_tlv_rule(unit->type);//根据tlv的type查找规则
        // 如果规则不存在或是无效的结果字段，或者正在等待die但当前单元不是die类型，则返回错误
        if (rule == NULL || rule->result_field == RK_RESULT_FIELD_COUNT ||
            rule->result_field == RK_RESULT_FIELD_PROGRESS ||
            (awaiting_die && unit->type != TLV_TYPE_RESULT_DIE)) return -1;
        // Value宽度和取值已由payload_length校验；这里按小端读取，再按字段类别处理。
        uint64_t value = read_le(tlv_unit_get_data(frame, unit), unit->data_len);
        if (rule->result_field == RK_RESULT_FIELD_ADDRESS) {
            pending_address = value;
            awaiting_die = 1U;
        } else if (rule->result_field == RK_RESULT_FIELD_DIE) {
            if (!awaiting_die || value >= RK_RESULT_DIE_COUNT ||
                decoded.failure_count >= RK_RESULT_DIE_COUNT ||
                (seen_dies & (1U << value)) != 0U) return -1;
            /* 零地址显式表示该颗PASS；仍登记die，防止PASS明细绕过重复检查。 */
            if (pending_address != 0U) {
                rk_failure_detail_t *failure = &decoded.failures[decoded.failure_count++];
                failure->error_address = pending_address;
                failure->die = (uint8_t)value;
            }
            seen_dies |= (uint8_t)(1U << value);
            awaiting_die = 0U;
        } else {
            uint8_t field_bit = (uint8_t)(1U << rule->result_field);
            if ((seen & field_bit) != 0U) return -1;
            seen |= field_bit;
            values[rule->result_field] = value;
        }
    }

    const uint8_t required = (1U << RK_RESULT_FIELD_COMMAND) |
                             (1U << RK_RESULT_FIELD_FAIL) |
                             (1U << RK_RESULT_FIELD_TIME);
    if (awaiting_die || seen != required ||
        ((values[RK_RESULT_FIELD_FAIL] != 0U) != (decoded.failure_count != 0U))) return -1;
    decoded.command_id = (uint16_t)values[RK_RESULT_FIELD_COMMAND];
    decoded.failed = (uint8_t)values[RK_RESULT_FIELD_FAIL];
    decoded.elapsed_ms = values[RK_RESULT_FIELD_TIME];
    /* 整帧通过后一次提交，两组明细中的坏项不能留下部分有效结果。 */
    *result = decoded;
    return 0;
}

int rk_report_decode(const tlv_frame_t *frame, rk_report_t *report)
{
    if (report == NULL) return -1;
    memset(report, 0, sizeof(*report));
    if (payload_length(frame) < 0) return -1;
    /* 先汇总并校验整帧，失败保持全零；残缺详情不得借FAIL/进度掩盖。 */
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
    const uint8_t details_mask = (1U << RK_RESULT_FIELD_PROGRESS) - 1U;
    const uint8_t command_bit = 1U << RK_RESULT_FIELD_COMMAND;
    const uint8_t progress_bit = 1U << RK_RESULT_FIELD_PROGRESS;
    const uint8_t progress_mask = command_bit | progress_bit;
    /* 进度必须带算法号；只有这两个已知字段时不是残缺的最终详情。 */
    if ((seen & progress_bit) != 0U && (seen & command_bit) == 0U) return -1;
    if (seen != progress_mask &&
        (seen & details_mask) != 0U && (seen & details_mask) != details_mask) return -1;
    if (seen & command_bit) {
        report->result.command_id = (uint16_t)values[RK_RESULT_FIELD_COMMAND];
    }
    if ((seen & details_mask) == details_mask) {
        report->fields |= RK_REPORT_DETAILS;
        report->result.error_address = values[RK_RESULT_FIELD_ADDRESS];
        report->result.die = (uint8_t)values[RK_RESULT_FIELD_DIE];
        report->result.elapsed_ms = values[RK_RESULT_FIELD_TIME];
    }
    if (seen & (1U << RK_RESULT_FIELD_PROGRESS)) {
        report->fields |= RK_REPORT_PROGRESS;
        report->progress_percent = (uint8_t)values[RK_RESULT_FIELD_PROGRESS];
    }
    if (seen & (1U << RK_RESULT_FIELD_FAIL)) {
        report->fields |= RK_REPORT_FAIL;
        report->result.failed = (uint8_t)values[RK_RESULT_FIELD_FAIL];
    }
    return 0;
}
