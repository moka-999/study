/*
 * ===================================================================
 * RK 通信任务 — MCU Master, 逻辑串口 ↔ RK3588 (TLV 协议)
 * ===================================================================
 *
 * 职责:
 *   1. 收 USART1 字节 → TLV 状态机 → 解析帧
 *   2. RK3588 响应帧 → 匹配 pending, 回传 pc_seq → PC (相同 CmdID)
 *   3. 命令转发: PC→RK 测试命令 (pcToRk_Queue) → 打包 → USART1
 *   4. 超时重传: 500ms 无响应重发, 最多 3 次 (指数退避)
 *
 * 心跳由 hb_task 独立发送, 防止被本任务阻塞.
 *
 * Seq 体系:
 *   g_rk_seq_counter — MCU→RK3588 统一序列号 (心跳 + 命令帧)
 *   pc_seq           — PC 帧的原始序列号, 响应时回传以匹配 PC 端
 *
 * 当前 uart_router slot 0:
 *   阿波罗 V2: USART1 + CH340→PC (用于协议对测)
 *   B1:        需切换为 UART4 或 USART2 + RS0102→RK3588（待真机确认）
 * ===================================================================
 */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "uart_router.h"
#include "state_machine.h"
#include "dispatch.h"
#include "hb_task.h"
#include "led.h"
#include <string.h>

/* ── 外部 ── */
extern QueueHandle_t rkToPc_Queue;
extern QueueHandle_t pcToRk_Queue;

/* ── 常量 ── */
#define RK_RESP_TIMEOUT_MS  500
#define RK_MAX_RETRIES      3

/* ── 待确认队列项 ── */
typedef struct {
    uint8_t  frame[TLV_FRAME_MAX];
    uint16_t len;
    uint64_t seq;           /* MCU→RK 序列号 (匹配) */
    uint64_t pc_seq;        /* PC 原始序列号 (回传) */
    uint32_t sent_tick;
    uint8_t  retries;
    uint8_t  active;
} rk_pending_t;

static rk_pending_t _pending[4];
static uint8_t      _pending_cnt = 0;

/* ═══════════════════════════════════════════════════════════════════
 * 待确认队列管理
 * ═══════════════════════════════════════════════════════════════════ */

static int _pending_add(const uint8_t *frame, uint16_t len,
                         uint64_t seq, uint64_t pc_seq)
{
    for (int i = 0; i < 4; i++) {
        if (!_pending[i].active) {
            memcpy(_pending[i].frame, frame, len);
            _pending[i].len       = len;
            _pending[i].seq       = seq;
            _pending[i].pc_seq    = pc_seq;
            _pending[i].sent_tick = xTaskGetTickCount();
            _pending[i].retries   = 0;
            _pending[i].active    = 1;
            if (i >= _pending_cnt) _pending_cnt = (uint8_t)(i + 1);
            return i;
        }
    }
    return -1;
}

/* 是否有空闲 pending 槽 (方案B: 转发前先确认能登记跟踪, 避免"发出去了却没人认领") */
static int _pending_has_slot(void)
{
    for (int i = 0; i < 4; i++) {
        if (!_pending[i].active) return 1;
    }
    return 0;
}

static int _pending_take(uint64_t seq, uint64_t *pc_seq)
{
    if (!pc_seq) return 0;
    for (int i = 0; i < 4; i++) {
        if (_pending[i].active && _pending[i].seq == seq) {
            *pc_seq = _pending[i].pc_seq;
            _pending[i].active = 0;
            return 1;
        }
    }
    return 0;
}

static int _pending_check_timeout(uint32_t now)
{
    for (int i = 0; i < 4; i++) {
        if (!_pending[i].active) continue;
        uint32_t timeout;
        if (_pending[i].retries >= RK_MAX_RETRIES) {
            /* 重试已用尽: 再等一个固定超时周期后放弃 */
            timeout = RK_RESP_TIMEOUT_MS;
        } else {
            /* 指数退避: 第 N 次重试的超时阈值 = 500ms << retries (500/1000/2000ms) */
            timeout = (uint32_t)(RK_RESP_TIMEOUT_MS << _pending[i].retries);
        }
        uint32_t elapsed = now - _pending[i].sent_tick;
        if (elapsed >= pdMS_TO_TICKS(timeout))
            return i;
    }
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════
 * 主任务
 * ═══════════════════════════════════════════════════════════════════ */

void StartRkTask(void *argument)
{
    uint8_t    buf[UART_RX_BUF_SIZE];
    tlv_sm_t   sm;
    (void)argument;

    tlv_sm_init(&sm);
    memset(_pending, 0, sizeof(_pending));

    for (;;)
    {
        uint32_t now = xTaskGetTickCount();

        /* ── ① 接收 USART1 数据 → TLV 状态机 ── */
        int len;
        while ((len = uart_poll_rx(0, buf, sizeof(buf))) > 0)
        {
            for (int i = 0; i < len; i++) {
                int r = tlv_sm_feed(&sm, buf[i]);
                if (r == 1) {
                    tlv_frame_t frame;
                    tlv_sm_get_frame(&sm, &frame);

                    for (uint8_t u = 0; u < frame.count; u++) {
                        tlv_unit_t *unit = &frame.units[u];

                        if (unit->type == TLV_TYPE_HEARTBEAT) {
                            /* RK3588 心跳响应: 更新存活状态 */
                            led_toggle(LED_HEARTBEAT);

                        } else if (unit->type == TLV_TYPE_CMD) {
                            /* RK 响应 (相同 CmdID): 匹配 pending, 取回 PC 原始 seq */
                            uint64_t pc_seq;

                            if (_pending_take(frame.sequence, &pc_seq)) {
                                /* 匹配成功: RK3588 响应 → 回传 PC */
                                if (rkToPc_Queue) {
                                    const uint8_t *d = tlv_unit_get_data(&frame, unit);
                                    rk_msg_t msg;
                                    msg.cmd      = unit->cmd;
                                    msg.data_len = unit->data_len;
                                    msg.sequence = pc_seq;
                                    memcpy(msg.data, d, unit->data_len);
                                    xQueueSend(rkToPc_Queue, &msg, 0);
                                }
                            } else {
                                /* 未匹配 pending 的 CMD 帧 = 重复/陈旧的 RK 响应
                                 * (重传导致的迟到重复响应). 绝不能回 ACK——否则 RK 会把
                                 * ACK 当新命令再回响应, 形成 MCU↔RK 的 ACK↔响应
                                 * ping-pong 死循环 (观测到单 seq 重传 1207 次). 静默丢弃. */
                            }
                        }
                    }
                }
            }
        }

        /* 半帧超时复位: 截断帧卡死后恢复 */
        tlv_sm_check_timeout(&sm, RK_RESP_TIMEOUT_MS);

        /* ── ② 超时重传检查 (非阻塞: 靠 sent_tick+retries 的退避超时调度) ── */
        {
            int idx = _pending_check_timeout(now);
            if (idx >= 0) {
                rk_pending_t *p = &_pending[idx];
                if (p->retries < RK_MAX_RETRIES) {
                    /* 无论发送成败都推进 retries/sent_tick:
                     * 若 TX 环满 uart_send 失败, 若不推进, 下一轮(10ms)
                     * 又会重试同一条 → 热重传风暴; 推进后重传按退避超时
                     * 调度且被 RK_MAX_RETRIES 封顶. */
                    (void)uart_send(0, p->frame, p->len);
                    p->retries++;
                    p->sent_tick = xTaskGetTickCount();
                } else {
                    p->active = 0;  /* 超限放弃 */
                }
            }
        }

        /* ── ③ PC→RK 命令转发: 用 g_rk_seq_counter 打包 → USART1 + 待确认 ──
         * 方案B: 先确认有空闲 pending 槽才取队列转发; 满了就留在队列等下一轮.
         * 这样每条发出去的命令必然被登记跟踪, 响应必能匹配回 PC,
         * 从根上消除"发出去了但 pending 满了 → 响应匹配不到"的丢响应隐患. */
        {
            pc2rk_msg_t fwd;
            while (_pending_has_slot() &&
                   xQueueReceive(pcToRk_Queue, &fwd, 0) == pdTRUE) {
                uint8_t frame[TLV_FRAME_MAX];

                /* 原子获取并递增共享序列号 */
                taskENTER_CRITICAL();
                uint64_t seq = g_rk_seq_counter++;
                taskEXIT_CRITICAL();

                int flen = tlv_pack_cmd(fwd.cmd, fwd.data, fwd.data_len,
                                        seq, frame, sizeof(frame));
                if (flen > 0) {
                    if (uart_send(0, frame, (uint16_t)flen) == 0) {
                        _pending_add(frame, (uint16_t)flen,
                                     seq, fwd.pc_seq);
                    } else {
                        /* TX 环满发送失败: 命令已从队列取出, 必须塞回队首,
                         * 否则这条命令静默丢失. 刚出队空出 1 槽, SendToFront 必成.
                         * break 让下一轮(10ms)重试, 不会在本轮空转. */
                        xQueueSendToFront(pcToRk_Queue, &fwd, 0);
                        break;
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
