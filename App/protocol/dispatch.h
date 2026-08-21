/*
 * ===================================================================
 * TLV 命令分发 — 查表路由
 * ===================================================================
 *
 * 路由:
 *   PC→MCU:  帧 unpack → tlv_dispatch()
 *            ├─ 本地命令 (reboot 等) → MCU 直接执行 + ACK
 *            └─ 测试命令 (0x0001-0x0024) → pcToRk_Queue → rk_task → RK3588
 *                                          MCU 先回 ACK → PC
 *
 *   RK→MCU:  响应帧 (与请求相同 CmdID) → rkToPc_Queue → TCP → PC
 *
 * CmdID 空间 (2字节, Value[0:2]):
 *   0x0001-0x0010  测试命令 (gtest-*)
 *   0x0011         reserved
 *   0x0012-0x0014  March 测试
 *   0x0015-0x0024  IDD 测试
 *   0x0025-0x003D  reserved
 *   0x003E         重启 RK3588
 *   0x003F         帧错误通知
 *   0x0040-0xFFFF  后续拓展
 * ===================================================================
 */
#ifndef __TLV_DISPATCH_H
#define __TLV_DISPATCH_H

#include <stdint.h>
#include "frame.h"

/* ── 命令 ID (2字节) ──
 *
 * RK 返回相同 CmdID, 通过 Params[0] 状态区分命令/结果:
 *   请求: Params[0] = 测试参数
 *   结果: Params[0] = 状态码 (暂定 0x01=PASS, 其他=失败)
 */
enum {
    /* 测试命令 (透传 RK3588, 结果同 CmdID) */
    TLV_CMD_GTEST_START  = 0x0001,
    TLV_CMD_GTEST_END    = 0x0010,
    /* 0x0011 = reserved */
    TLV_CMD_MARCHX       = 0x0012,
    TLV_CMD_MARCHC_MINUS = 0x0013,
    TLV_CMD_MARCHC_PLUS  = 0x0014,
    TLV_CMD_IDD_START    = 0x0015,
    TLV_CMD_IDD_END      = 0x0024,
    /* 0x0025-0x003D = reserved (原结果范围已废弃, 结果回相同CmdID) */
    /* 控制 */
    TLV_CMD_REBOOT       = 0x003E,
    TLV_CMD_FRAME_ERROR  = 0x003F,
};

/* ── 响应状态码 (Params[0], 1 字节) ── */
enum {
    TLV_ST_PASS = 0x01,  /* 测试结果: 通过 */
    TLV_ST_FAIL = 0x02,  /* 测试结果: 失败 */
    TLV_ST_ACK  = 0x03,  /* 命令已接收/已转发 (ACK) */
    TLV_ST_BUSY = 0x04,  /* 转发队列满, 命令被丢弃 (背压) */
};

/* ── PC→RK 队列消息 ── */
typedef struct {
    uint16_t cmd;
    uint8_t  data[TLV_CMD_DATA_MAX];
    uint16_t data_len;
    uint64_t pc_seq;       /* PC 原始 seq, 响应时回传 */
} pc2rk_msg_t;

/* ── 注册/分发 ── */
void tlv_register_handler(uint16_t cmd, tlv_handler_t handler);

/* 遍历帧内所有 TLV 单元, 逐个匹配命令表并调用 handler
   返回: 最后一个 handler 的返回值, 或 0 */
int  tlv_dispatch(const tlv_frame_t *req, uint8_t *resp, uint16_t max);

void tlv_dispatch_init(void);

#endif
