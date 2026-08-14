/*
 * ===================================================================
 * TLV 协议帧 — 打包/解包
 * ===================================================================
 *
 * 帧格式:
 *   [0x5A] [Len:2B BE] [HeaderCRC:2B LE] [Seq:8B] [TLV...N] [FrameCRC:2B LE] [0xA5]
 *
 *   字段        字节    覆盖
 *   ──────────────────────────────────────────
 *   Sync        1      0x5A
 *   Len         2      TLV 数据总字节数 (不含 Seq)
 *   HeaderCRC   2      CRC16(0x5A + Len_H + Len_L)
 *   Seq         8      序列号 (BigEndian)
 *   TLV data    N      Type + TLV_Len + Value 的串联
 *   FrameCRC    2      CRC16(Len_H + Len_L + TLV data), 不含 Seq
 *   Footer      1      0xA5
 *
 * TLV 单元:
 *   [Type:2B BE] [TLV_Len:2B BE] [Value:TLV_Len]
 *
 *   Type = 0x0000 → 心跳 (Value 为空)
 *   Type = 0x5000 → 命令容器 (Value = [CmdID:2B BE] [Params:N])
 *   Type = 其他   → 后续拓展预留
 *
 *   TLV_Len = Value 字节数 = 2 + len(Params)
 *
 * CRC:
 *   HeaderCRC = crc16_modbus([0x5A, Len_H, Len_L])
 *   FrameCRC  = crc16_modbus([Len_H, Len_L]) +
 *               crc16_modbus_continue([TLV data N 字节])
 *               (不含 Seq, 中间跳过)
 * ===================================================================
 */
#ifndef __TLV_FRAME_H
#define __TLV_FRAME_H

#include <stdint.h>

/* ── 帧常量 ── */
#define TLV_SYNC_HEADER     0x5A
#define TLV_SYNC_FOOTER     0xA5

/* ── TLV Type ── */
#define TLV_TYPE_HEARTBEAT  0x0000  /* 心跳: Value 为空              */
#define TLV_TYPE_CMD        0x5000  /* 命令容器: Value=[CmdID:2B][参数] */

/* ── 容量 ──
 *   Len 字段 = TLV 字节数, 不含 Seq. 最大取 1024.
 *   满载单 TLV: Type(2) + TLV_Len(2) + CmdID(2) + data_max = 6 + data_max ≤ 1024
 *   → data_max = 1018
 *   TLV_FRAME_MAX = 1(Sync) + 2(Len) + 2(HCRC) + 8(Seq) + 1024(TLV) + 2(FCRC) + 1(Footer)
 *                 = 1040
 * ── */
#define TLV_PAYLOAD_MAX     1024    /* Len 最大值                      */
#define TLV_CMD_DATA_MAX    (TLV_PAYLOAD_MAX - 6)  /* 1018, 单 TLV 参数上限 */
#define TLV_FRAME_MAX       (TLV_PAYLOAD_MAX + 16) /* 1040              */
#define TLV_MAX_PER_FRAME   16      /* 一帧最多 TLV 单元数              */

struct tlv_frame;

/* ── 单个 TLV 单元 ── */
typedef struct {
    uint16_t type;          /* TLV Type (0x0000=心跳, 0x5000=命令)   */
    uint16_t cmd;           /* CmdID (Type=0x5000 时 Value[0:2])     */
    uint16_t data_off;      /* 参数在 tlv_frame.payload[] 中的偏移    */
    uint16_t data_len;      /* 参数长度 (Value 总长 - 2)             */
} tlv_unit_t;

/* ── 帧解包结果 ── */
typedef struct tlv_frame {
    tlv_unit_t units[TLV_MAX_PER_FRAME];
    uint8_t    payload[TLV_PAYLOAD_MAX];
    uint8_t    count;
    uint16_t   payload_used;
    uint64_t   sequence;
} tlv_frame_t;

/* ── 从共享 buffer 获取 TLV 单元参数指针 ── */
static inline const uint8_t *tlv_unit_get_data(const tlv_frame_t *f,
                                                const tlv_unit_t *u)
{
    return f->payload + u->data_off;
}

/* ── 命令处理回调 ── */
typedef int (*tlv_handler_t)(const tlv_frame_t *frame,
                             const tlv_unit_t *unit,
                             uint8_t *resp, uint16_t max);

/* ── RK→PC 队列消息 (跨任务传递解析结果) ── */
typedef struct {
    uint16_t cmd;
    uint8_t  data[TLV_CMD_DATA_MAX];
    uint16_t data_len;
    uint64_t sequence;
} rk_msg_t;

/* ── 打包 API ── */

/* 打包命令帧: Type=0x5000, Value=[cmd:2B][data:N]
   seq:     MCU→RK 序列号
   返回:    帧总长度, 0=缓冲区不够 */
int tlv_pack_cmd(uint16_t cmd,
                 const uint8_t *data, uint16_t data_len,
                 uint64_t seq,
                 uint8_t *out, uint16_t out_max);

/* 打包心跳帧: Type=0x0000, Value 为空
   返回: 帧总长度, 0=缓冲区不够 */
int tlv_pack_heartbeat(uint64_t seq,
                       uint8_t *out, uint16_t out_max);

/* ── 解包 API ── */

/* 解包: frame_buf 从 Seq 起始, frame_len = Len 值 (TLV 字节数)
   返回: 成功解析的 TLV 单元数, -1=错误 */
int tlv_unpack(const uint8_t *frame_buf, uint16_t frame_len,
               tlv_frame_t *out);

/* ── 工具 ── */

/* 从帧 buf[1:3] 提取 Length (BigEndian → host) */
static inline uint16_t tlv_get_length(const uint8_t *buf)
{ return ((uint16_t)buf[0] << 8) | buf[1]; }

/* 提取 8 字节序列号 (BigEndian → host) */
uint64_t tlv_get_sequence(const uint8_t *buf);

#endif
