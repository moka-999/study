/*
 * ===================================================================
 * MCU-RK通信任务 — MCU Master, 逻辑串口 ↔ RK3588 (TLV协议)
 * ===================================================================
 *
 * 职责:
 *   1. 收逻辑串口0字节 → TLV状态机 → 解析帧。
 *   2. RK响应匹配pending后交给旧PC–TLV结果队列；正式PC JSON尚无消费适配。
 *   3. 旧PC–TLV命令队列(pcToRk_Queue) → 分配RK Seq → 打包发送。
 *   4. 超时重传: 500ms 无响应重发, 最多 3 次 (指数退避)
 *
 * 心跳由hb_task独立调度，两者仍共享UART发送互斥锁。
 *
 * 协议边界：本任务只生成和消费RK侧u64 Seq，不解析PC固定头或JSON。
 * pc_seq只是旧PC–TLV通路的关联值。新PC发送Seq仅为本端流水号，业务关联
 * 规则另行冻结；不能沿用旧字段当作已完成的新PC/RK请求映射。
 *
 * Seq 体系:
 *   rk_link_next_sequence() — MCU→RK3588统一序号(心跳+新命令)。
 *   pc_seq — 旧TLV关联值，不是已实现的新PC u32 Seq映射。
 *
 * 当前 uart_router slot 0:
 *   阿波罗 V2: USART1 + CH340→PC (用于协议对测)
 *   B1:        需切换为 UART4 或 USART2 + RS0102→RK3588（待真机确认）
 * ===================================================================
 */
#include "main.h"
#include "FreeRTOS.h"
#include "ota_app.h"
#include "task.h"
#include "queue.h"
#include "uart_router.h"
#include "state_machine.h"
#include "dispatch.h"
#include "rk_link.h"
#include "led.h"
#include <string.h>

/* ── 常量 ── */
#define RK_RESP_TIMEOUT_MS  500
#define RK_MAX_RETRIES      3
#define RK_PENDING_CAPACITY 4

/* ── 待确认队列项 ── */
typedef struct {
    uint8_t  frame[TLV_FRAME_MAX];
    uint16_t len;
    uint64_t seq;           /* MCU→RK 序列号 (匹配) */
    uint64_t pc_seq;        /* 历史TLV关联值；不是新PC协议的u32映射。 */
    uint32_t sent_tick;
    uint8_t  retries;
    uint8_t  active;
} rk_pending_t;

/* 仅本任务读写，无需跨任务锁；心跳不占用pending槽。 */
static rk_pending_t _pending[RK_PENDING_CAPACITY];

/**
 * @brief 将已发送RK帧复制到首个空闲pending槽，供超时重传。
 * @param[in] frame 非NULL完整帧，在调用期间有效。
 * @param[in] len 帧字节数，调用方保证不超过TLV_FRAME_MAX。
 * @param[in] seq RK响应匹配序号。
 * @param[in] pc_seq 历史PC–TLV队列关联值，不是新PC响应Seq。
 * @return 槽索引0..RK_PENDING_CAPACITY-1；无槽返回-1。
 * @note 仅RK任务访问，不检查输入指针/长度；成功后原帧缓冲可复用。
 */
static int _pending_add(const uint8_t *frame, uint16_t len,
                         uint64_t seq, uint64_t pc_seq)
{
    for (int i = 0; i < RK_PENDING_CAPACITY; i++) {
        if (!_pending[i].active) {
            memcpy(_pending[i].frame, frame, len);
            _pending[i].len       = len;
            _pending[i].seq       = seq;
            _pending[i].pc_seq    = pc_seq;
            _pending[i].sent_tick = xTaskGetTickCount();
            _pending[i].retries   = 0;
            _pending[i].active    = 1;
            return i;
        }
    }
    return -1;
}

/**
 * @brief 检查是否还有可登记的RK命令跟踪槽。
 * @return 1有空槽；0已满，此时任务暂不取走队列命令。
 * @note 仅RK任务读写pending，此检查本身不保留槽位。
 */
static int _pending_has_slot(void)
{
    for (int i = 0; i < RK_PENDING_CAPACITY; i++) {
        if (!_pending[i].active) return 1;
    }
    return 0;
}

/**
 * @brief 按RK Seq查找并立即释放pending，取回历史关联值。
 * @param[in] seq 待匹配RK序号。
 * @param[out] pc_seq 非NULL输出；未匹配时保持原值。
 * @return 1匹配并释放；0无匹配或输出为NULL。
 * @note 不核对CmdID；首个匹配单元即消耗槽，结果队列投递发生在释放之后。
 *       当前未实现多阶段结果或队列满时保留结果的生命周期。
 */
static int _pending_take(uint64_t seq, uint64_t *pc_seq)
{
    /* 当前只按RK Seq匹配，不核对CmdID；首个匹配单元即释放pending。
     * 这是现有单回复基线，不代表已支持ACK后再返回最终结果或多单元上报。 */
    if (!pc_seq) return 0;
    for (int i = 0; i < RK_PENDING_CAPACITY; i++) {
        if (_pending[i].active && _pending[i].seq == seq) {
            *pc_seq = _pending[i].pc_seq;
            _pending[i].active = 0;
            return 1;
        }
    }
    return 0;
}

/**
 * @brief 查找第一个达到重传/放弃跟踪阈值的槽。
 * @param[in] now 当前FreeRTOS tick，不是毫秒；与sent_tick使用同一时基。
 * @return 首个超时槽索引；没有返回-1。
 * @note 不修改槽。重试阈值500/1000/2000ms；3次重试用尽后再等500ms，
 *       上层才放弃跟踪。按槽顺序检查，不是按最早到期排序。
 */
static int _pending_check_timeout(uint32_t now)
{
    for (int i = 0; i < RK_PENDING_CAPACITY; i++) {
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

/**
 * @brief RK通信任务入口，消费串口帧并管理历史命令队列与重传。
 * @param[in] argument 未使用，可为NULL。
 * @note 启动前须创建pcToRk_Queue并注册/启动UART槽0。任务独占pending；
 *       CRC就绪后仍检查整帧TLV结构。结果队列尚未接正式PC JSON消费，
 *       不能将本任务运行解释为PC–RK业务闭环完成；循环等待10ms，不正常返回。
 */
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

        /* ── ① 逻辑串口0接收块 → RK TLV状态机 ── */
        int len;
        while ((len = uart_poll_rx(0, buf, sizeof(buf))) > 0)
        {
            for (int i = 0; i < len; i++) {
                int r = tlv_sm_feed(&sm, buf[i]);
                if (r == 1) {
                    tlv_frame_t frame;
                    if (tlv_sm_get_frame(&sm, &frame) < 0) {
                        /* CRC正确但TLV损坏：整帧拒绝，继续找下一帧；
                         * 不消费解析前缀、不释放pending，也不向RK回ACK。 */
                        continue;
                    }

                    for (uint8_t u = 0; u < frame.count; u++) {
                        tlv_unit_t *unit = &frame.units[u];

                        if (unit->type == TLV_TYPE_HEARTBEAT) {
                            /* 收到RK心跳仅翻转指示灯；离线判定尚未接入。 */
                            led_toggle(LED_HEARTBEAT);

                        } else if (unit->type == TLV_TYPE_CMD) {
                            /* RK响应匹配pending，取回历史队列关联值。 */
                            uint64_t pc_seq;

                            if (_pending_take(frame.sequence, &pc_seq)) {
                                /* 写入旧结果队列；正式PC JSON尚未消费此队列。
                                 * TODO: 当前先释放pending，队列满时会丢结果；
                                 * 新业务接入前须定义结果保留与超时失败策略。 */
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
                                /* 当前未匹配pending的CMD一律静默丢弃；可能是迟到
                                 * 重复包，也可能是尚未支持的异步/第二阶段结果，
                                 * 不能仅凭未匹配就断言它是旧包。绝不能回ACK，否则RK会把
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
                    p->active = 0;  /* 当前仅放弃跟踪，链路故障上报尚未接入。 */
                }
            }
        }

        /* ── ③ 历史队列命令转发：分配独立RK序号 → 串口 + 待确认 ──
         * 方案B: 先确认有空闲 pending 槽才取队列转发; 满了就留在队列等下一轮.
         * 本任务独占pending，发送成功后登记；只保证跟踪槽不超配，
         * 不代表结果队列投递或PC响应已经完成。 */
        {
            pc2rk_msg_t fwd;
            while (_pending_has_slot() &&
                   xQueueReceive(pcToRk_Queue, &fwd, 0) == pdTRUE) {
                uint8_t frame[TLV_FRAME_MAX];

                uint64_t seq = rk_link_next_sequence();

                int flen = tlv_pack_cmd(fwd.cmd, fwd.data, fwd.data_len,
                                        seq, frame, sizeof(frame));
                if (flen > 0) {
                    if (uart_send(0, frame, (uint16_t)flen) == 0) {
                        _pending_add(frame, (uint16_t)flen,
                                     seq, fwd.pc_seq);
                    } else {
                        /* TX 环满发送失败: 命令已从队列取出, 必须塞回队首,
                         * 否则这条命令静默丢失。多生产者并发时空槽可能被抢占，
                         * 此历史路径仍需在新业务适配接入时完善回插失败策略。
                         * break 让下一轮(10ms)重试, 不会在本轮空转. */
                        xQueueSendToFront(pcToRk_Queue, &fwd, 0);
                        break;
                    }
                }
            }
        }

        ota_app_progress(OTA_TASK_RK);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
