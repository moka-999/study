/*
 * ===================================================================
 * TLV 命令分发 — 查表路由实现
 * ===================================================================
 *
 *   测试命令(0x0001-0x0024) → pcToRk_Queue → rk_task → RK3588
 *     MCU 先回 ACK(Params[0]=0x03), 结果同 CmdID 异步回报
 *
 *   本地命令(reboot 等) → MCU 直接执行 + 响应
 *
 *   RK 返回相同 CmdID, 通过 Params 状态区分命令/结果
 * ===================================================================
 */
#include "main.h"
#include "dispatch.h"
#include "frame.h"
#include "crc.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include <string.h>

/* ── RK3588 复位 GPIO ── */
#ifndef RK_RST_PORT
#define RK_RST_PORT  GPIOG
#define RK_RST_PIN   GPIO_PIN_12
#endif

/* ── 外部依赖 ── */
extern void *rkToPc_Queue;
extern void *pcToRk_Queue;

/* ── 处理器原型 ── */
static int _h_test_cmd(const tlv_frame_t *frame, const tlv_unit_t *unit,
                        uint8_t *resp, uint16_t max);
static int _h_reboot(const tlv_frame_t *frame, const tlv_unit_t *unit,
                      uint8_t *resp, uint16_t max);
static int _h_frame_error(const tlv_frame_t *frame, const tlv_unit_t *unit,
                           uint8_t *resp, uint16_t max);

/* ── 命令表 (key: uint16_t CmdID) ── */
typedef struct {
    uint16_t       cmd;
    tlv_handler_t  handler;
} cmd_entry_t;

static cmd_entry_t _cmd_table[64];
static uint8_t     _cmd_count = 0;

/* ═══════════════════════════════════════════════════════════════════
 * 注册
 * ═══════════════════════════════════════════════════════════════════ */

void tlv_register_handler(uint16_t cmd, tlv_handler_t handler)
{
    if (_cmd_count >= 64) return;
    _cmd_table[_cmd_count].cmd     = cmd;
    _cmd_table[_cmd_count].handler = handler;
    _cmd_count++;
}

/* ═══════════════════════════════════════════════════════════════════
 * 初始化: 注册内置处理器
 * ═══════════════════════════════════════════════════════════════════ */

void tlv_dispatch_init(void)
{
    /* 测试命令 0x0001-0x0024 → 转发 RK3588 */
    for (uint16_t c = 0x0001; c <= 0x0010; c++) tlv_register_handler(c, _h_test_cmd);
    for (uint16_t c = 0x0012; c <= 0x0014; c++) tlv_register_handler(c, _h_test_cmd);
    for (uint16_t c = 0x0015; c <= 0x0024; c++) tlv_register_handler(c, _h_test_cmd);

    /* 控制命令 */
    tlv_register_handler(TLV_CMD_REBOOT,      _h_reboot);
    tlv_register_handler(TLV_CMD_FRAME_ERROR, _h_frame_error);
}

/* ═══════════════════════════════════════════════════════════════════
 * 查表分发 — 遍历帧内所有 TLV 单元
 * ═══════════════════════════════════════════════════════════════════ */

int tlv_dispatch(const tlv_frame_t *req, uint8_t *resp, uint16_t max)
{
    int last_ret = 0;

    for (uint8_t u = 0; u < req->count; u++) {
        const tlv_unit_t *unit = &req->units[u];

        /* 只处理命令容器 (Type=0x5000) */
        if (unit->type != TLV_TYPE_CMD) continue;

        int handled = 0;
        for (uint8_t i = 0; i < _cmd_count; i++) {
            if (_cmd_table[i].cmd == unit->cmd) {
                last_ret = _cmd_table[i].handler(req, unit, resp, max);
                handled = 1;
                break;
            }
        }

        if (!handled) {
            const uint8_t *d = tlv_unit_get_data(req, unit);
            last_ret = tlv_pack_cmd(TLV_CMD_FRAME_ERROR,
                                    d, unit->data_len,
                                    req->sequence, resp, max);
        }
    }

    return last_ret;
}

/* ═══════════════════════════════════════════════════════════════════
 * 内置处理器
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * 测试命令透传: PC → MCU → RK3588
 *   unit->cmd   = 2字节 CmdID
 *   unit->data  = 参数 (不含 CmdID)
 */
static int _h_test_cmd(const tlv_frame_t *frame, const tlv_unit_t *unit,
                        uint8_t *resp, uint16_t max)
{
    const uint8_t *d = tlv_unit_get_data(frame, unit);

    /* 原始命令 → pcToRk_Queue → rk_task 用 MCU seq 打包 → RK3588 */
    uint8_t status = TLV_ST_ACK;   /* 默认: 已入队转发 */
    if (pcToRk_Queue) {
        pc2rk_msg_t msg;
        msg.cmd      = unit->cmd;
        msg.data_len = unit->data_len;
        msg.pc_seq   = frame->sequence;
        memcpy(msg.data, d, msg.data_len);
        if (xQueueSend(pcToRk_Queue, &msg, 0) != pdPASS) {
            /* 转发队列满: 命令被丢弃, 如实回 BUSY 而非假 ACK, 让 PC 端感知背压 */
            status = TLV_ST_BUSY;
        }
    }

    return tlv_pack_cmd(unit->cmd, &status, 1, frame->sequence, resp, max);
}

/*
 * 重启 RK3588: MCU GPIO 拉低 RK3588 NPOR
 *
 * 注意: dispatch 在 tcp_task 上下文同步执行, 这里不能用 vTaskDelay 阻塞 —
 *       6s 阻塞会让 TCP server 停止收帧/转发队列 (rkToPc_Queue 8 槽溢出丢 RK 上报),
 *       与 rk_task 曾有的"阻塞重传"是同一类 bug。
 *       改用一次性软件定时器: 立即回 ACK, 拉低复位脚, 由定时器到期后拉高释放。
 */
static TimerHandle_t _rk_rst_timer = NULL;

static void _rk_rst_timer_cb(TimerHandle_t t)
{
    (void)t;
    HAL_GPIO_WritePin(RK_RST_PORT, RK_RST_PIN, GPIO_PIN_SET);
}

static int _h_reboot(const tlv_frame_t *frame, const tlv_unit_t *unit,
                      uint8_t *resp, uint16_t max)
{
    uint8_t mode = 0;
    if (unit->data_len >= 1) {
        const uint8_t *d = tlv_unit_get_data(frame, unit);
        mode = d[0];
    }
    uint32_t pulse_ms = mode ? 6000u : 500u;

    if (_rk_rst_timer == NULL) {
        _rk_rst_timer = xTimerCreate("rkRst", pdMS_TO_TICKS(pulse_ms),
                                     pdFALSE, NULL, _rk_rst_timer_cb);
    }

    HAL_GPIO_WritePin(RK_RST_PORT, RK_RST_PIN, GPIO_PIN_RESET);
    if (_rk_rst_timer != NULL) {
        /* xTimerReset: 定时器休眠则启动, 运行中则重新计时 (覆盖并发 reboot) */
        xTimerChangePeriod(_rk_rst_timer, pdMS_TO_TICKS(pulse_ms), 0);
        xTimerReset(_rk_rst_timer, 0);
    } else {
        /* 定时器创建失败 (极罕见): 立即释放复位脚, 至少不卡死 RK 复位态 */
        HAL_GPIO_WritePin(RK_RST_PORT, RK_RST_PIN, GPIO_PIN_SET);
    }

    uint8_t status = TLV_ST_ACK;
    return tlv_pack_cmd(unit->cmd, &status, 1, frame->sequence, resp, max);
}

/*
 * 帧错误通知
 */
static int _h_frame_error(const tlv_frame_t *frame, const tlv_unit_t *unit,
                           uint8_t *resp, uint16_t max)
{
    (void)frame;
    uint8_t status = TLV_ST_ACK;
    return tlv_pack_cmd(unit->cmd, &status, 1, frame->sequence, resp, max);
}
