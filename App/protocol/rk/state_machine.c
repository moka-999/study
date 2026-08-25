/*
 * ===================================================================
 * MCU-RK TLV 帧解析状态机 — 逐字节实现
 * ===================================================================
 *
 * 仅用于MCU-RK串口链路；PC-MCU TCP必须使用pc_protocol_stream_t。
 *
 * 帧布局:
 *   [0] 0x5A            Sync
 *   [1] Len_H           Len = TLV 字节数 (不含 Seq)
 *   [2] Len_L
 *   [3] CRC_L           Header CRC16 (LE) — 覆盖 0x5A + Len(2)
 *   [4] CRC_H
 *   [5..12]             Seq (8B BE)
 *   [13..12+Len]        TLV 数据 (Len 字节)
 *   [13+Len] CRC_F_L    Frame CRC16 (LE) — 覆盖 Len(2) + TLV 数据
 *   [14+Len] CRC_F_H
 *   [15+Len] 0xA5       Footer
 *
 * 校验:
 *   Header CRC = crc16_modbus([0x5A, Len_H, Len_L])
 *   Frame CRC  = crc16_modbus([Len_H, Len_L])
 *              + crc16_modbus_continue([TLV data Len 字节])
 *              (跳过 HeaderCRC + Seq)
 * ===================================================================
 */
#include "state_machine.h"
#include "crc.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

void tlv_sm_init(tlv_sm_t *sm)
{
    memset(sm, 0, sizeof(*sm));
    sm->state = TLV_SM_IDLE;
}

void tlv_sm_reset(tlv_sm_t *sm)
{
    sm->state  = TLV_SM_IDLE;
    sm->pos    = 0;
    sm->expect = 0;
}

/* ── 伪同步恢复: 在已收集的 buf[1..pos-1] 中搜索 0x5A ── */
static int _resync(tlv_sm_t *sm)
{
    uint16_t i;
    for (i = 1; i < sm->pos; i++) {
        if (sm->buf[i] == TLV_SYNC_HEADER) {
            uint16_t remain = sm->pos - i;
            memmove(sm->buf, sm->buf + i, remain);
            sm->pos    = remain;
            sm->expect = 4;
            return 1;
        }
    }
    return 0;
}

int tlv_sm_check_timeout(tlv_sm_t *sm, uint32_t timeout_ms)
{
    /* 仅当半帧进行中 (非 IDLE) 且自最后字节起超过 timeout_ms 才复位 */
    if (sm->state == TLV_SM_IDLE) {
        return 0;
    }
    uint32_t now = xTaskGetTickCount();
    if ((now - sm->last_tick) >= pdMS_TO_TICKS(timeout_ms)) {
        tlv_sm_reset(sm);
        return 1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * 核心: 逐字节喂入
 * ═══════════════════════════════════════════════════════════════════ */

int tlv_sm_feed(tlv_sm_t *sm, uint8_t byte)
{
    sm->last_tick = xTaskGetTickCount();   /* 记录最后字节时间戳, 供超时复位用 */
    if (sm->pos >= TLV_FRAME_MAX) {
        tlv_sm_reset(sm);
        return -1;
    }
    sm->buf[sm->pos++] = byte;

    switch (sm->state) {

    /* ─── IDLE: 等待 0x5A ─── */
    case TLV_SM_IDLE:
        if (byte == TLV_SYNC_HEADER) {
            sm->state  = TLV_SM_HEADER;
            sm->expect = 4;   /* 还需 Len(2) + HeaderCRC(2) */
        } else {
            sm->pos = 0;
        }
        break;

    /* ─── HEADER: 收集 Len(2) + HeaderCRC(2), 验证头 CRC ─── */
    case TLV_SM_HEADER:
        if (sm->pos < 5) break;

        {
            uint16_t calc = crc16_modbus(sm->buf, 3);  /* 0x5A + Len(2) */
            uint16_t exp  = sm->buf[3] | ((uint16_t)sm->buf[4] << 8);

            if (calc == exp) {
                uint16_t plen = tlv_get_length(sm->buf + 1);  /* TLV 字节数 */
                if (plen == 0 || plen > TLV_PAYLOAD_MAX) {
                    if (!_resync(sm)) tlv_sm_reset(sm);
                    break;
                }
                sm->state  = TLV_SM_DATA;
                /* 还需 Seq(8) + TLV(plen) + FCRC(2) + Footer(1)
                 * 但 CRC/Footer 各在自己的状态里处理
                 * DATA 态收集 Seq + TLV = 8 + plen 字节 */
                sm->expect = 8 + plen;
            } else {
                if (!_resync(sm)) tlv_sm_reset(sm);
            }
        }
        break;

    /* ─── DATA: 收集 Seq(8B) + TLV 数据 (plen 字节) ─── */
    case TLV_SM_DATA:
        if (sm->pos < 5 + sm->expect) break;

        sm->state  = TLV_SM_CRC;
        sm->expect = 2;   /* 还需 FrameCRC(2) */
        break;

    /* ─── CRC: 收集 FrameCRC(2), 验证帧 CRC ─── */
    case TLV_SM_CRC:
        {
            uint16_t plen = tlv_get_length(sm->buf + 1);
            /* CRC 在 buf[5+8+plen] = buf[13+plen] */
            if (sm->pos < 13 + plen + 2) break;

            /* Frame CRC: Len(2) + TLV 数据 (plen 字节, 从 buf[13] 开始) */
            uint16_t calc = crc16_modbus(sm->buf + 1, 2);       /* Len(2)     */
            calc = crc16_modbus_continue(calc, sm->buf + 13, plen); /* + TLVs */
            uint16_t crc_pos = 13 + plen;
            uint16_t exp  = sm->buf[crc_pos] | ((uint16_t)sm->buf[crc_pos + 1] << 8);

            if (calc == exp) {
                sm->state  = TLV_SM_FOOTER;
                sm->expect = 1;   /* 还需 0xA5 */
            } else {
                tlv_sm_reset(sm);
                return -1;
            }
        }
        break;

    /* ─── FOOTER: 验证 0xA5 ─── */
    case TLV_SM_FOOTER:
        if (sm->buf[sm->pos - 1] == TLV_SYNC_FOOTER) {
            tlv_sm_reset(sm);
            return 1;              /* 完整帧就绪 */
        } else {
            tlv_sm_reset(sm);
            return -1;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * 取出已解析帧
 * ═══════════════════════════════════════════════════════════════════ */

void tlv_sm_get_frame(tlv_sm_t *sm, tlv_frame_t *frame)
{
    uint16_t plen = tlv_get_length(sm->buf + 1);
    /* buf+5 = Seq 起始, plen = TLV 字节数 */
    tlv_unpack(sm->buf + 5, plen, frame);
}
