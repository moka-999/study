/*
 * ===================================================================
 * 旧PC–TLV业务适配器 — 查表路由实现
 * ===================================================================
 *
 * 此为旧PC–TLV分发入口，正式PC TCP任务已不调用它；保留用于迁移核对。
 * 下述ACK/队列行为不是现行PC JSON协议。未来业务适配须显式定义转换，
 * 不能把本分发器重新接回PC原始帧接收路径。
 *
 *   测试命令(0x0001-0x0024) → pcToRk_Queue → rk_task → RK3588
 *     旧适配器先向旧PC入口回ACK(Params[0]=0x03)，不是RK的受理应答。
 *
 *   本地命令(reboot 等) → MCU 直接执行 + 响应
 *
 * 旧模拟测试采用相同CmdID回结果；rk_task实际仅按RK Seq匹配pending。
 * 原文的独立结果码未作废；ACK后再返回最终结果等生命周期尚未实现。
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
#include "rk_link.h"
#include <string.h>

/* ── 旧V2适配器使用的复位GPIO，不是B1的RK复位定义 ──
 * B1的PG12实际连接PHY ETH_nRST，绝不能用于RK复位。
 * B1正常重启应由pwr_ctrl管理PC6/PC7/PG11的安全掉电/上电，时序单独确认。
 * 此处只有注释警告，没有B1编译期阻断；不能把该handler接入B1正式业务。
 */
#ifndef RK_RST_PORT
#define RK_RST_PORT  GPIOG
#define RK_RST_PIN   GPIO_PIN_12
#endif

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
 * 旧PC–TLV测试命令转发：保留CmdID/Params，交给rk_task重建RK帧。
 *   unit->cmd   = 2字节 CmdID
 *   tlv_unit_get_data(frame, unit) = 参数起始地址（不含CmdID）
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
 * 旧V2复位脉冲实现：拉低RK_RST_PIN，定时释放；不是B1正常重启流程。
 *
 * 历史TLV入口曾在tcp_task上下文同步调用本handler；正式TCP现已切到PC JSON，
 * 但同步handler本身仍不能用vTaskDelay阻塞。一次性软件定时器仅避免阻塞，
 * 不解决上方PG12引脚归属问题，也不代表该实现可移植到B1。
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
