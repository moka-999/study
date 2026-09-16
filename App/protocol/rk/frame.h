/*
 * ===================================================================
 * MCU-RK TLV 当前实现基线 — 打包/解包
 * ===================================================================
 *
 * 仅用于MCU-RK串口链路，不用于PC-MCU TCP；PC协议见../pc/pc_protocol.h。
 * 两侧Seq属于独立命名空间，业务层负责关联，禁止原始帧透传。
 * 下述线格式用于当前固件回归；与外部RK文档的Type/CRC/结果码差异仍需
 * 双方确认，不能把本实现当作已宣布废弃其他设计的依据。
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
 *   Type = 其他   → 校验TLV结构后跳过，不解释其业务含义
 *
 *   命令容器 TLV_Len = 2 + len(Params)；心跳 TLV_Len = 0。
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
#define TLV_PAYLOAD_MIN     4       /* 至少包含Type(2)+TLV_Len(2)       */
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

/**
 * @brief 获取已解包单元参数在frame内的借用指针。
 * @param[in] f 有效解包结果，非NULL。
 * @param[in] u 属于f且偏移/长度有效的单元，非NULL。
 * @return f->payload内的参数起点，有效字节数为u->data_len，不可释放。
 * @note 此内联访问器不校验指针或边界；f修改或失效后指针不得继续使用。
 */
static inline const uint8_t *tlv_unit_get_data(const tlv_frame_t *f,
                                                const tlv_unit_t *u)
{
    return f->payload + u->data_off;
}

/* ── 命令处理回调 ── */
typedef int (*tlv_handler_t)(const tlv_frame_t *frame,
                             const tlv_unit_t *unit,
                             uint8_t *resp, uint16_t max);

/* ── 旧PC–TLV适配器的结果队列载荷，正式PC JSON尚未消费此队列 ── */
typedef struct {
    uint16_t cmd;
    uint8_t  data[TLV_CMD_DATA_MAX];
    uint16_t data_len;
    uint64_t sequence;
} rk_msg_t;

/* ── 打包 API ── */

/**
 * @brief 打包一个Type=0x5000的MCU到RK命令帧。
 * @param[in] cmd RK命令号，写入Value前两字节。
 * @param[in] data 至少可读data_len字节；长度为0时可为NULL。
 * @param[in] data_len 参数字节数，调用方须保证不超过TLV_CMD_DATA_MAX。
 * @param[in] seq 独立RK序号；重传沿用原序号。
 * @param[out] out 非NULL输出，不得与data重叠。
 * @param[in] out_max out容量，单位字节；完整帧需要22 + data_len字节。
 * @return 完整帧字节数；容量不足返回0且不写输出。
 * @note 当前仅检查计算后的容量，未检查空指针或data_len协议上限；必须先
 *       满足上述前提，避免16位长度运算回绕。此API不发送帧。
 */
int tlv_pack_cmd(uint16_t cmd,
                 const uint8_t *data, uint16_t data_len,
                 uint64_t seq,
                 uint8_t *out, uint16_t out_max);

/**
 * @brief 打包Type=0x0000、空Value的RK心跳帧。
 * @param[in] seq 独立RK序号。
 * @param[out] out 非NULL输出缓冲；函数未做空指针校验。
 * @param[in] out_max 缓冲字节容量。
 * @return 20成功；容量不足返回0且不写输出。
 */
int tlv_pack_heartbeat(uint64_t seq,
                       uint8_t *out, uint16_t out_max);

/* ── 解包 API ── */

/**
 * @brief 解包RK TLV载荷，任一单元结构损坏时拒绝整帧。
 * @param[in] frame_buf 从Seq起始，非NULL且至少可读8 + frame_len字节。
 * @param[in] frame_len Len字段，即TLV区字节数，范围4..1024，不含Seq。
 * @param[out] out 非NULL完整结果，参数数据复制到其payload，不借用输入。
 * @return >=0为输出单元数，未知Type全部跳过时允许0；-1表示参数/结构/容量
 *         错误，此时out非NULL则整份清零，不交付已解析前缀。
 * @note 调用方先校验帧CRC和尾；所有线上TLV（含未知Type/心跳）合计1..16个。
 *       无命令时多个心跳合并为一个；输入不得与out重叠。
 */
int tlv_unpack(const uint8_t *frame_buf, uint16_t frame_len,
               tlv_frame_t *out);

/* ── 工具 ── */

/**
 * @brief 从两个大端字节读取TLV区长度。
 * @param[in] buf 指向Length字段本身，至少可读2字节，不是整帧起点。
 * @return 主机字节序长度值；不做空指针或协议范围检查。
 */
static inline uint16_t tlv_get_length(const uint8_t *buf)
{ return ((uint16_t)buf[0] << 8) | buf[1]; }

/**
 * @brief 从八个大端字节读取RK序号。
 * @param[in] buf 指向Seq字段，非NULL且至少可读8字节。
 * @return 主机字节序uint64_t序号；不检查输入边界。
 */
uint64_t tlv_get_sequence(const uint8_t *buf);

#endif
