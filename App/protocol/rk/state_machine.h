/*
 * ===================================================================
 * RK帧解析状态机 — 逐字节喂入，调用方显式取帧
 * ===================================================================
 *
 * 目标用途: 仅解析 MCU-RK 串口上的 0x5A/0xA5 TLV 字节流。
 * PC-MCU TCP使用../pc/pc_state_machine.h，禁止把JSON帧送入本状态机。
 *
 * 状态转移:
 *   IDLE → HEADER → DATA → CRC → FOOTER → (complete) → IDLE
 *
 * 用法:
 *   tlv_sm_t sm;
 *   tlv_sm_init(&sm);
 *   while (recv_byte(&b)) {
 *     int r = tlv_sm_feed(&sm, b);
 *     if (r > 0) {
 *       tlv_frame_t f;
 *       if (tlv_sm_get_frame(&sm, &f) >= 0) {
 *         // ... 只处理完整且TLV结构有效的帧 ...
 *       }
 *     }
 *   }
 *
 * 超时: 调用方通过 tlv_sm_check_timeout() 定期检查,
 *       半帧期间连续500ms没有新字节时复位；不是整帧总耗时限制。
 * ===================================================================
 */
#ifndef __TLV_STATE_MACHINE_H
#define __TLV_STATE_MACHINE_H

#include <stdint.h>
#include "frame.h"

/* ── 状态枚举 ── */
typedef enum {
    TLV_SM_IDLE   = 0,
    TLV_SM_HEADER = 1,  /* 收集 Len(2) + HeaderCRC(2) = 4B  */
    TLV_SM_DATA   = 2,  /* 收集 Sequence(8B) + TLV 数据      */
    TLV_SM_CRC    = 3,  /* 收集 FrameCRC(2)                  */
    TLV_SM_FOOTER = 4,  /* 验证 0xA5                         */
} tlv_sm_state_t;

/* ── 状态机实例 ── */
typedef struct {
    tlv_sm_state_t state;
    uint8_t  buf[TLV_FRAME_MAX];   /* 帧缓冲 (含 Sync+Len+CRC+Payload+Footer) */
    uint16_t pos;                   /* 当前写入位置                            */
    uint16_t expect;                /* 期望收集的字节数 (状态相关)              */
    uint32_t last_tick;             /* 最后收到字节的时间戳 (FreeRTOS tick)     */
    uint8_t  sync_pos;             /* 伪同步恢复: 0x5A在buf中的位置            */
} tlv_sm_t;

/* ── API ── */

/* 初始化状态机 */
void tlv_sm_init(tlv_sm_t *sm);

/* 喂入一个字节：0=继续，1=边界/CRC校验通过，-1=帧错误(已自动复位)。
 * 返回1后仍须检查get_frame返回值；CRC正确不保证内部TLV有效。 */
int  tlv_sm_feed(tlv_sm_t *sm, uint8_t byte);

/* 仅在feed返回1后、下一字节喂入前调用；sm必须是有效实例。
 * 返回>=0为输出单元数（未知Type可全部跳过）；-1为TLV解包失败，输出清零。 */
int tlv_sm_get_frame(tlv_sm_t *sm, tlv_frame_t *frame);

/* 超时复位：半帧状态下，自最后字节起超过timeout_ms → 强制归IDLE
 * 返回: 1=已复位, 0=未超时 */
int  tlv_sm_check_timeout(tlv_sm_t *sm, uint32_t timeout_ms);

/* 强制复位 */
void tlv_sm_reset(tlv_sm_t *sm);

#endif
