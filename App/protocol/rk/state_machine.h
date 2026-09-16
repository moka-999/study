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

/**
 * @brief 清零并初始化RK字节流状态机为IDLE。
 * @param[out] sm 非NULL实例；函数不做空指针检查。
 */
void tlv_sm_init(tlv_sm_t *sm);

/**
 * @brief 消费一个RK串口字节并更新最后接收tick。
 * @param[in,out] sm 非NULL已初始化实例，任务上下文串行访问。
 * @param[in] byte 下一个接收字节。
 * @return 0继续（包括头CRC/长度错误后的重同步）；1帧边界及CRC通过；
 *         -1缓冲溢出、帧CRC或尾错误且已复位。
 * @note 返回1后必须在下一字节前调用get_frame并检查结果；CRC正确不保证
 *       内部TLV有效。本入口不校验sm是否为NULL。
 */
int  tlv_sm_feed(tlv_sm_t *sm, uint8_t byte);

/**
 * @brief 解包刚刚由feed收齐的RK帧。
 * @param[in] sm 非NULL实例，必须紧跟feed返回1且在下一字节之前调用。
 * @param[out] frame 非NULL输出；解包失败时整份清零。
 * @return >=0为输出单元数，允许未知Type被全部跳过；-1参数或TLV结构错误。
 * @note 不额外判断帧是否就绪；成功结果复制载荷，不依赖sm后续生命周期。
 */
int tlv_sm_get_frame(tlv_sm_t *sm, tlv_frame_t *frame);

/**
 * @brief 检查半帧的字节间超时并按需复位。
 * @param[in,out] sm 非NULL实例，任务上下文调用。
 * @param[in] timeout_ms 超时阈值，内部用pdMS_TO_TICKS转换。
 * @return 1非IDLE状态距最后字节已达到阈值并复位；0无需复位。
 * @note 这是字节间间隔，不是整帧耗时；本函数不检查空指针。
 */
int  tlv_sm_check_timeout(tlv_sm_t *sm, uint32_t timeout_ms);

/**
 * @brief 放弃RK收帧进度并回到IDLE。
 * @param[in,out] sm 非NULL实例；函数不做空指针检查。
 * @note 仅重置state/pos/expect，保留缓冲供成功feed后立即取帧。
 */
void tlv_sm_reset(tlv_sm_t *sm);

#endif
