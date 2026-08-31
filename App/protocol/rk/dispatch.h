/*
 * ===================================================================
 * 旧PC–TLV业务适配器 — 查表路由
 * ===================================================================
 *
 * 协议边界:
 *   frame/state_machine仍用于现行MCU-RK串口；本分发器则属于旧PC–TLV入口。
 *   正式PC TCP已使用../pc/下的18字节固定头+JSON，不调用本分发器。
 *   新业务须显式转换语义，不能重新接入旧PC原始TLV帧路径。
 *
 * 历史路由（待PC正式业务层迁移后收敛）:
 *   PC→MCU:  旧TLV帧 unpack → tlv_dispatch()
 *            ├─ 本地命令 (reboot 等) → MCU 直接执行 + ACK
 *            └─ 测试命令 (0x0001-0x0024) → pcToRk_Queue → rk_task → RK3588
 *                                          MCU 先回 ACK → PC
 *
 *   RK→MCU:  Type=0x5000且Seq匹配pending → rkToPc_Queue → 旧TCP路径
 *            正式PC JSON任务尚未消费该结果队列。
 *
 * CmdID 空间 (2字节, Value[0:2]):
 *   0x0001-0x0010  测试命令 (gtest-*)
 *   0x0011         reserved
 *   0x0012-0x0014  March 测试
 *   0x0015-0x0024  IDD 测试
 *   0x0025-0x0028  本适配器未定义
 *   0x0029-0x003D  原文结果/状态命令范围（含保留项），未在本枚举声明
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
 * 旧模拟测试采用“结果回相同CmdID，Params[0]表示状态”的约定。
 * rk_task实际只按RK Seq匹配pending，不要求响应CmdID与请求相同；
 * 原文的独立结果CmdID并未被双方宣布废弃，结果载荷须与RK工程师统一。
 */
enum {
    /* 旧适配器向RK转发的测试命令；此枚举不规定结果CmdID。 */
    TLV_CMD_GTEST_START  = 0x0001,
    TLV_CMD_GTEST_END    = 0x0010,
    /* 0x0011 = reserved */
    TLV_CMD_MARCHX       = 0x0012,
    TLV_CMD_MARCHC_MINUS = 0x0013,
    TLV_CMD_MARCHC_PLUS  = 0x0014,
    TLV_CMD_IDD_START    = 0x0015,
    TLV_CMD_IDD_END      = 0x0024,
    /* 0x0025-0x0028未定义；0x0029-0x003D原文结果/状态码留待双方冻结。 */
    /* 控制 */
    TLV_CMD_REBOOT       = 0x003E,
    TLV_CMD_FRAME_ERROR  = 0x003F,
};

/* ── 旧TLV适配器/模拟测试状态码 (Params[0], 1字节)，不是PC JSON应答合同 ── */
enum {
    TLV_ST_PASS = 0x01,  /* 测试结果: 通过 */
    TLV_ST_FAIL = 0x02,  /* 测试结果: 失败 */
    TLV_ST_ACK  = 0x03,  /* 命令已接收/已转发 (ACK) */
    TLV_ST_BUSY = 0x04,  /* 转发队列满, 命令被丢弃 (背压) */
};

/*
 * 旧PC–TLV内部队列消息，pc_seq只保存旧路径的关联值。
 * 新PC发送Seq仅为本端流水号，业务请求关联规则须另行定义；此结构不代表
 * 新PC JSON业务已经接入，也不能将PC Seq和RK Seq相互替代。
 */
typedef struct {
    uint16_t cmd;
    uint8_t  data[TLV_CMD_DATA_MAX];
    uint16_t data_len;
    uint64_t pc_seq;       /* 旧PC–TLV的u64关联值，不是新PC u32发送流水号。 */
} pc2rk_msg_t;

/* ── 注册/分发 ── */
void tlv_register_handler(uint16_t cmd, tlv_handler_t handler);

/* 遍历帧内所有TLV单元，逐个查表调用handler。
 * 多个handler复用并覆盖同一resp缓冲，不具备多响应聚合能力。
 * 返回最后一个handler的返回值，未处理时返回0。 */
int  tlv_dispatch(const tlv_frame_t *req, uint8_t *resp, uint16_t max);

void tlv_dispatch_init(void);

#endif
