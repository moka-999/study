/*
 * ===================================================================
 * TLV 协议帧 — 打包/解包实现
 * ===================================================================
 */
#include "frame.h"
#include "crc.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 * 打包: 命令帧
 *
 *   [0x5A][Len:2B][HCRC:2B][Seq:8B][Type=0x5000][TLV_Len:2B][CmdID:2B][data][FCRC:2B][0xA5]
 *
 *   Len      = 6 + data_len  (TLV 总字节)
 *   TLV_Len  = 2 + data_len  (Value 字节)
 *   FrameCRC = CRC16(Len:2B) + CRC16_continue(TLV data)
 * ═══════════════════════════════════════════════════════════════════ */

int tlv_pack_cmd(uint16_t cmd,
                 const uint8_t *data, uint16_t data_len,
                 uint64_t seq,
                 uint8_t *out, uint16_t out_max)
{
    uint16_t tlv_len     = 2 + data_len;  /* Value = CmdID(2) + params */
    uint16_t payload_len = 4 + tlv_len;   /* Type(2) + TLV_Len(2) + Value */
    uint16_t total_len   = 1 + 2 + 2 + 8 + payload_len + 2 + 1;  /* 16 + payload */

    if (total_len > out_max) return 0;

    uint16_t pos = 0;

    /* ── Header ── */
    out[pos++] = TLV_SYNC_HEADER;               /* [0]  Sync      */
    out[pos++] = (uint8_t)(payload_len >> 8);   /* [1]  Len_H     */
    out[pos++] = (uint8_t)(payload_len);        /* [2]  Len_L     */

    uint16_t hcrc = crc16_modbus(out + pos - 3, 3);  /* 0x5A + Len(2) */
    out[pos++] = (uint8_t)(hcrc);               /* [3]  HCRC_L    */
    out[pos++] = (uint8_t)(hcrc >> 8);          /* [4]  HCRC_H    */

    /* ── Sequence ── */
    out[pos++] = (uint8_t)(seq >> 56);          /* [5]  */
    out[pos++] = (uint8_t)(seq >> 48);          /* [6]  */
    out[pos++] = (uint8_t)(seq >> 40);          /* [7]  */
    out[pos++] = (uint8_t)(seq >> 32);          /* [8]  */
    out[pos++] = (uint8_t)(seq >> 24);          /* [9]  */
    out[pos++] = (uint8_t)(seq >> 16);          /* [10] */
    out[pos++] = (uint8_t)(seq >> 8);           /* [11] */
    out[pos++] = (uint8_t)(seq);                /* [12] */

    /* ── TLV: Type=0x5000 + TLV_Len + Value=[CmdID:2B][params] ── */
    uint16_t tlv_start = pos;                   /* 记 TLV 起始 → Frame CRC */

    out[pos++] = 0x50;                          /* Type_H   */
    out[pos++] = 0x00;                          /* Type_L   */
    out[pos++] = (uint8_t)(tlv_len >> 8);       /* TLV Len_H */
    out[pos++] = (uint8_t)(tlv_len);            /* TLV Len_L */

    /* Value: CmdID (2B BE) */
    out[pos++] = (uint8_t)(cmd >> 8);
    out[pos++] = (uint8_t)(cmd);

    /* Value: params */
    if (data_len > 0) {
        memcpy(out + pos, data, data_len);
        pos += data_len;
    }

    /* ── Frame CRC: Len(2B) + TLVs ── */
    uint16_t fcrc = crc16_modbus(out + 1, 2);   /* CRC Len(2)   */
    fcrc = crc16_modbus_continue(fcrc, out + tlv_start,
                                  (uint16_t)(pos - tlv_start)); /* + TLVs */
    out[pos++] = (uint8_t)(fcrc);               /* FCRC_L       */
    out[pos++] = (uint8_t)(fcrc >> 8);          /* FCRC_H       */

    out[pos++] = TLV_SYNC_FOOTER;
    return pos;
}

/* ═══════════════════════════════════════════════════════════════════
 * 打包: 心跳帧
 *
 *   [0x5A][Len=4][HCRC][Seq:8B][Type=0x0000][TLV_Len=0][FCRC][0xA5]
 *
 *   Len = 4  (Type:2 + TLV_Len:2, Value 空)
 * ═══════════════════════════════════════════════════════════════════ */

int tlv_pack_heartbeat(uint64_t seq,
                       uint8_t *out, uint16_t out_max)
{
    uint16_t payload_len = 4;   /* Type(2) + TLV_Len(2), Value 空 */
    uint16_t total_len   = 1 + 2 + 2 + 8 + payload_len + 2 + 1;

    if (total_len > out_max) return 0;

    uint16_t pos = 0;

    /* Header */
    out[pos++] = TLV_SYNC_HEADER;
    out[pos++] = 0x00;                          /* Len_H = 0  */
    out[pos++] = 0x04;                          /* Len_L = 4  */

    uint16_t hcrc = crc16_modbus(out + pos - 3, 3);
    out[pos++] = (uint8_t)(hcrc);
    out[pos++] = (uint8_t)(hcrc >> 8);

    /* Sequence */
    out[pos++] = (uint8_t)(seq >> 56);
    out[pos++] = (uint8_t)(seq >> 48);
    out[pos++] = (uint8_t)(seq >> 40);
    out[pos++] = (uint8_t)(seq >> 32);
    out[pos++] = (uint8_t)(seq >> 24);
    out[pos++] = (uint8_t)(seq >> 16);
    out[pos++] = (uint8_t)(seq >> 8);
    out[pos++] = (uint8_t)(seq);

    /* TLV: Type=0x0000, TLV_Len=0 */
    uint16_t tlv_start = pos;
    out[pos++] = 0x00;                          /* Type_H   */
    out[pos++] = 0x00;                          /* Type_L   */
    out[pos++] = 0x00;                          /* Len_H    */
    out[pos++] = 0x00;                          /* Len_L    */

    /* Frame CRC: Len(2) + TLVs */
    uint16_t fcrc = crc16_modbus(out + 1, 2);
    fcrc = crc16_modbus_continue(fcrc, out + tlv_start, (uint16_t)(pos - tlv_start));
    out[pos++] = (uint8_t)(fcrc);
    out[pos++] = (uint8_t)(fcrc >> 8);

    out[pos++] = TLV_SYNC_FOOTER;
    return pos;
}

/* ═══════════════════════════════════════════════════════════════════
 * 解包
 *
 * frame_buf 指向 Seq 起始处: [Seq:8B] [TLV...]
 * frame_len = Len (TLV 字节数, 不含 Seq)
 * ═══════════════════════════════════════════════════════════════════ */

uint64_t tlv_get_sequence(const uint8_t *buf)
{
    uint64_t seq = 0;
    for (int i = 0; i < 8; i++)
        seq = (seq << 8) | buf[i];
    return seq;
}

int tlv_unpack(const uint8_t *frame_buf, uint16_t frame_len,
               tlv_frame_t *out)
{
    /* frame_buf[0..7] = Seq, frame_buf[8..8+frame_len-1] = TLVs */
    if (frame_len < 4) return -1;  /* 至少 1 个空 TLV */

    memset(out, 0, sizeof(*out));
    out->sequence = tlv_get_sequence(frame_buf);

    uint16_t offset = 8;           /* 跳过 Seq */
    uint16_t end    = 8 + frame_len;
    uint8_t  has_heartbeat = 0;

    while (offset + 4 <= end && out->count < TLV_MAX_PER_FRAME) {
        uint16_t type  = ((uint16_t)frame_buf[offset] << 8)
                       | frame_buf[offset + 1];
        uint16_t t_len = ((uint16_t)frame_buf[offset + 2] << 8)
                       | frame_buf[offset + 3];
        offset += 4;

        if (offset + t_len > end) return -1;  /* TLV 超长 */

        /* ── 心跳: Type=0x0000 ── */
        if (type == TLV_TYPE_HEARTBEAT) {
            has_heartbeat = 1;
            offset += t_len;
            continue;
        }

        /* ── 命令容器: Type=0x5000, Value=[CmdID:2B][params:N] ── */
        if (type == TLV_TYPE_CMD && t_len >= 2) {
            uint16_t cmd   = ((uint16_t)frame_buf[offset] << 8)
                           | frame_buf[offset + 1];
            uint16_t d_len = t_len - 2;  /* 参数长度 */

            if (out->payload_used + d_len > TLV_PAYLOAD_MAX)
                break;

            tlv_unit_t *u = &out->units[out->count];
            u->type     = TLV_TYPE_CMD;
            u->cmd      = cmd;
            u->data_off = out->payload_used;
            u->data_len = d_len;
            if (d_len > 0) {
                memcpy(out->payload + out->payload_used,
                       frame_buf + offset + 2, d_len);
                out->payload_used += d_len;
            }
            out->count++;
        }
        /* 其他 Type: 跳过, 留待拓展 */

        offset += t_len;
    }

    /* 纯心跳帧: 无命令 TLV 时记为心跳 */
    if (has_heartbeat && out->count == 0) {
        out->units[0].type = TLV_TYPE_HEARTBEAT;
        out->count = 1;
    }

    return (int)out->count;
}
