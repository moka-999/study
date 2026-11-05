/* RK串口任务：收发零TLV心跳、缓存参考进度、校验完整双颗粒结果后一次交付。
 * 结果超时后RK执行状态未知，暂停后续命令；仅匹配的迟到结果或B1断电解除。
 * UART/DMA由既有路由层负责；无PC JSON、供电时序或测试算法实现。
 */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "uart_router.h"
#include "state_machine.h"
#include "rk_link.h"
#include "task_watchdog.h"
#if defined(B1_RK_UART)
#include "pwr_ctrl.h"
#endif
#include <string.h>

/* MCU向RK发送心跳间隔1000ms；本轮有效普通心跳可确认启动，不决定算法完成。 */
#define RK_HEARTBEAT_INTERVAL_MS 1000U
/* 每轮最多取8个RX块（含断流通知）；持续来数据也必须让出CPU并推进心跳/超时。
 * 未取走的块留在UART队列，半帧保留在状态机中，不因达到配额而清空。
 */
#define RK_RX_BATCH_MAX 8U
/* 无应答模式留20ms发送间隔，覆盖RK的5ms轮询及主循环处理；不是生效确认。 */
#define RK_TIMING_GAP_MS 20U
static TickType_t s_timing_sent_tick;
static uint8_t s_timing_gap;

typedef struct {
    rk_request_t request;
    rk_response_t response;
    uint64_t sequence;        /* 本次下行测试命令Seq；仅作为接收窗口起点和内部诊断。 */
    uint64_t last_tx_sequence; /* 本次等待期间最近成功入UART队列的MCU Seq。 */
    uint64_t progress_sequence; /* RK进度携带的最近已收MCU Seq，仅作参考。 */
    TickType_t sent_tick;
    uint8_t waiting;       /* 正等待该Seq/命令的最终结果，含超时后的状态未知期。 */
    uint8_t timed_out;     /* 已判定TIMEOUT，可能仍待入队；不得再交付COMPLETE。 */
    uint8_t delivery;      /* 已完成但结果队列满时，保留副本直到交付。 */
    uint8_t recovered;     /* 迟到结果/断电已解除未知；须等TIMEOUT入队后再报恢复。 */
    uint8_t progress_valid; /* 新请求清零；1表示本次等待中收到过合法进度心跳。 */
    uint8_t progress_percent; /* 0..100，仅参考，不代表DDR-ready或测试结束。 */
} rk_pending_t;
static rk_pending_t s_pending;
/* 仅RK任务写；volatile保留调试器可读的s_rx_diagnostics符号，不逐帧打印日志。 */
static volatile rk_rx_diagnostics_t s_rx_diagnostics;

int rk_link_get_rx_diagnostics(rk_rx_diagnostics_t *out)
{
    if (out == NULL) {
        return -1;
    }
    taskENTER_CRITICAL();
    *out = s_rx_diagnostics;
    taskEXIT_CRITICAL();
    return 0;
}

/* 记录拒绝时的窗口及原因；只读取pending，不改变结果匹配或队列交付。 */
static void note_rx_reject(rk_rx_reject_reason_t reason, const tlv_frame_t *frame,
                           uint16_t received_command)
{
    taskENTER_CRITICAL();
    if (s_rx_diagnostics.rejected_total != UINT32_MAX) {
        ++s_rx_diagnostics.rejected_total;
    }
    if (s_rx_diagnostics.counts[reason] != UINT32_MAX) {
        ++s_rx_diagnostics.counts[reason];
    }
    s_rx_diagnostics.last_reason = reason;
    s_rx_diagnostics.last_tick = xTaskGetTickCount();
    s_rx_diagnostics.frame_valid = frame != NULL;
    s_rx_diagnostics.received_sequence = frame != NULL ? frame->sequence : 0U;
    s_rx_diagnostics.tlv_count = frame != NULL ? frame->count : 0U;
    s_rx_diagnostics.waiting = s_pending.waiting;
    s_rx_diagnostics.command_sequence = s_pending.waiting ? s_pending.sequence : 0U;
    s_rx_diagnostics.last_tx_sequence = s_pending.waiting ? s_pending.last_tx_sequence : 0U;
    s_rx_diagnostics.expected_command = s_pending.waiting ? s_pending.request.command_id : 0U;
    s_rx_diagnostics.received_command = received_command;
    taskEXIT_CRITICAL();
}

/* 格式不完整时按是否含进度字段分类；不把缺字段的最终结果误记成进度。 */
static void note_rx_format_reject(const tlv_frame_t *frame)
{
    rk_rx_reject_reason_t reason = RK_RX_REJECT_RESULT;
    for (unsigned i = 0U; i < frame->count && i < TLV_MAX_PER_FRAME; ++i) {
        if (frame->units[i].type == TLV_TYPE_PROGRESS) {
            reason = RK_RX_REJECT_PROGRESS;
            break;
        }
    }
    note_rx_reject(reason, frame, 0U);
}

/* 填写本地结果，实际入队由deliver_result完成；失败状态的算法结果保持清零。 */
static void finish_request(rk_response_status_t status)
{
    memset(&s_pending.response, 0, sizeof(s_pending.response));
    s_pending.response.cookie = s_pending.request.cookie;
    s_pending.response.sequence = s_pending.sequence;
    s_pending.response.status = status;
    s_pending.delivery = 1U;
}
/* 只由RK任务调用；一次最多交付TIMEOUT及其RECOVERED两个事件。
 * 队列满时保留原response，绝不让迟到结果覆盖尚未交付的TIMEOUT。
 */
static void deliver_result(void)
{
    if (rkResponseQueue == NULL) return;
    //是否有要交付的结果
    if (s_pending.delivery) {
        if (xQueueSend(rkResponseQueue, &s_pending.response, 0U) != pdTRUE) return;
        s_pending.delivery = 0U;
    }
    // 是否有待交付的恢复事件；先交付原超时响应，再通知等待状态已解除。
    if (s_pending.recovered) {
        s_pending.recovered = 0U;
        finish_request(RK_RESPONSE_RECOVERED);
        if (xQueueSend(rkResponseQueue, &s_pending.response, 0U) == pdTRUE) {
            s_pending.delivery = 0U;
        }
    }
}
/* timeout_ms=0无限等待；非0沿用有符号半周期以内的tick差值判断。 */
static int result_wait_expired(void)
{
    if (s_pending.request.timeout_ms == 0U || s_pending.timed_out) return 0;
    uint64_t ticks = (uint64_t)s_pending.request.timeout_ms * configTICK_RATE_HZ / 1000U;
    return (TickType_t)(xTaskGetTickCount() - s_pending.sent_tick) >= (TickType_t)ticks;
}
/* RK所有上报携带它最近收到的MCU Seq，不能严格等于测试命令或MCU最新发送值。
 * 只做发送窗口过滤：拒绝早于本次命令或超出已发送上界的Seq，允许传输中的滞后；
 * uint64无符号差支持计数回绕；完整结果还须匹配单在途的算法号。
 * 同算法的旧结果若被RK重新赋予当前窗口Seq，协议无法区分，不能据Seq宣称防重放。
 * 进度只保留观察值，不生成PC新字段；无效结果保持等待，不放行后续测试。
 */
static void accept_frame(const tlv_frame_t *frame)
{
    /* 启动查询独立于算法窗口；收到后立即回复，不等待普通心跳或测试请求。
     * 算法在途时不替意外重启的RK沿用旧启动参数，也不改变已有结果等待。
     */
    if (rk_is_frequency_query(frame)) {
        if (s_pending.waiting) return;
        uint32_t token;
        int64_t value = rk_link_frequency_get(&token);
        uint8_t wire[TLV_FREQUENCY_FRAME_SIZE];
        int length = tlv_pack_frequency(value, rk_link_next_sequence(), wire, sizeof(wire));
        int success = length == TLV_FREQUENCY_FRAME_SIZE && uart_send(0, wire, (uint16_t)length) == 0;
        rk_link_frequency_note_tx(token, success);
        return;
    }
#if defined(B1_RK_UART)
    /* 上电过渡期间只开放频率查询；心跳、进度和结果仍须ON，不绕过就绪门禁。 */
    if (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) return;
#endif
    /* 普通心跳无需有在途算法；启动编排等待它之后才会投递测试请求。 */
    // 启动就绪模块只检查普通心跳；双TLV进度和最终结果在下方分别处理。
    rk_link_ready_note_rx(frame);
    // 空指针直接返回；零TLV或兼容的单个0x0000空Value心跳不进入结果处理。
    if (frame == NULL || frame->count == 0U ||
        (frame->count == 1U && frame->units[0].type == TLV_TYPE_HEARTBEAT &&
         frame->units[0].data_len == 0U)) return;
    // 如果当前没有等待的请求，记录拒绝原因并返回
    if (!s_pending.waiting) {
        note_rx_reject(RK_RX_REJECT_NO_PENDING, frame, 0U);
        return;
    }
    //seq是否合规
    if ((uint64_t)(frame->sequence - s_pending.sequence) >
        (uint64_t)(s_pending.last_tx_sequence - s_pending.sequence)) {
        note_rx_reject(RK_RX_REJECT_SEQUENCE, frame, 0U);
        return;
    }
    /* 进度只接受5001+3016双TLV；旧版详情/独立FAIL不参与新结果拼接。 */
    //count=2 进度心跳包5001+3016
    if (frame->count == 2U) {
        rk_report_t report;
        if (rk_report_decode(frame, &report) != 0 || report.fields != RK_REPORT_PROGRESS) {
            note_rx_format_reject(frame);
        } else if (report.result.command_id != s_pending.request.command_id) {
            //检查是否是当前的算法号
            note_rx_reject(RK_RX_REJECT_ALGORITHM, frame, report.result.command_id);
        } else {
            s_pending.progress_percent = report.progress_percent;
            s_pending.progress_sequence = frame->sequence;
            s_pending.progress_valid = 1U;
        }
        return;
    }
    // 3/5/7 TLV最终结果包；必须有5001+1017+9002，最多两组9001+3001。
    rk_final_result_t result;
    if (rk_final_result_decode(frame, &result) != 0) {
        //格式不完整或字段重复，拒绝该帧
        note_rx_format_reject(frame);
        return;
    }
    if (result.command_id != s_pending.request.command_id) {
        //检查是否是当前的算法号
        note_rx_reject(RK_RX_REJECT_ALGORITHM, frame, result.command_id);
        return;
    }
    if (result_wait_expired()) {
        //是否超时
        s_pending.timed_out = 1U;
        finish_request(RK_RESPONSE_TIMEOUT);
    }
    // 合法最终结果已到达，结束等待；仍须交付响应或恢复事件后才能取新请求。
    s_pending.waiting = 0U;
    /* 已向业务层判超时的请求不能改成第二次成功；迟到结果仅证明RK已结束。 */
    if (s_pending.timed_out) {
        s_pending.recovered = 1U;
        return;
    }
    // 准备完整结果响应；随后由deliver_result送入队列，COMPLETE不等于DDR测试PASS。
    finish_request(RK_RESPONSE_COMPLETE);
    s_pending.response.result = result;
}
/* 每轮调用；只处理一个请求，UART整帧入队失败不会丢请求或无界重试。 */
static void process_request(void)
{
    // 先交付上轮结果，让pc任务取走
    deliver_result();
    // 如果上轮请求仍在等待中，检查是否超时
    if (s_pending.waiting) {
        if (result_wait_expired()) {
            s_pending.timed_out = 1U;
            finish_request(RK_RESPONSE_TIMEOUT);
        }
        return;
    }
    // 没有在途等待后，仍须确认旧响应/恢复事件已交付且队列可用，才能取新请求。
    if (s_pending.delivery || s_pending.recovered ||
        rkRequestQueue == NULL || rkResponseQueue == NULL) return;
    if (s_timing_gap) {
        if ((TickType_t)(xTaskGetTickCount() - s_timing_sent_tick) < pdMS_TO_TICKS(RK_TIMING_GAP_MS)) return;
        s_timing_gap = 0U;
    }
    //准备新的请求
    rk_request_t request;
    // 从队列中获取新的请求 并保存
    if (xQueueReceive(rkRequestQueue, &request, 0U) != pdTRUE) return;
    memset(&s_pending, 0, sizeof(s_pending));
    s_pending.request = request;
    // 检查请求的算法号是否有效，以及超时时间是否合理
    uint64_t ticks = (uint64_t)request.timeout_ms * configTICK_RATE_HZ / 1000U;
    int timing = rk_is_timing_command(request.command_id);
    if ((!timing && rk_command_find_by_id(request.command_id) == NULL) ||
        (timing ? request.timeout_ms != 0U : request.parameter_value != 0U) ||
        (request.timeout_ms != 0U && (ticks == 0U || ticks > INT32_MAX))) {
        finish_request(RK_RESPONSE_INVALID);
        return;
    }
    //分配新的Seq，打包请求帧，并发送到UART
    s_pending.sequence = rk_link_next_sequence();
    uint8_t wire[TLV_TIMING_FRAME_SIZE];
    int length = timing ? tlv_pack_timing(request.command_id, request.parameter_value,
                                        s_pending.sequence, wire, sizeof(wire)) :
                         tlv_pack_cmd(request.command_id, NULL, 0U,
                                      s_pending.sequence, wire, sizeof(wire));
    // 检查打包结果和发送结果，如果失败则标记TX_FAILED
    if (length != (int)(timing ? TLV_TIMING_FRAME_SIZE : TLV_CMD_FRAME_SIZE) ||
        uart_send(0, wire, (uint16_t)length) != 0) {
        /* uart_send失败保证未向TX环写入任何字节；不回插队列冒丢失风险。 */
        finish_request(RK_RESPONSE_TX_FAILED);
        return;
    }
    if (timing) {
        s_timing_sent_tick = xTaskGetTickCount();
        s_timing_gap = 1U;
        finish_request(RK_RESPONSE_SENT);
        return;
    }
    // 记录发送时间和最近发送的Seq，标记为等待状态
    s_pending.sent_tick = xTaskGetTickCount();
    s_pending.last_tx_sequence = s_pending.sequence;
    s_pending.waiting = 1U;
}
#if defined(B1_RK_UART)
/* B1掉电不静默清结果；未执行请求逐条返回POWER_OFF，不假装完成测试。 */
static void process_power_off(void)
{
    s_timing_gap = 0U;
    if (s_pending.waiting) {
        s_pending.waiting = 0U;
        if (s_pending.timed_out) s_pending.recovered = 1U;
        else finish_request(RK_RESPONSE_POWER_OFF);
    }
    deliver_result();
    if (!s_pending.delivery && !s_pending.recovered && rkRequestQueue != NULL &&
        xQueueReceive(rkRequestQueue, &s_pending.request, 0U) == pdTRUE) {
        s_pending.sequence = 0U;
        finish_request(RK_RESPONSE_POWER_OFF);
    }
}
#endif
/* B1启动早期可响应0x35，但不改变电源状态、不提前发送算法或普通心跳。 */
#if defined(B1_RK_UART)
static int startup_rx_allowed(void)
{
    pwr_ctrl_state_t state = pwr_ctrl_get_state();
    return state == PWR_CTRL_STATE_WAIT_FIRST_RAIL || state == PWR_CTRL_STATE_WAIT_SECOND_RAIL;
}
#endif
/* last_tick为上次发送尝试的RTOS tick，仅RK任务持有；初值为任务启动时刻。
 * 到期只尝试一帧，错过的周期不补发；失败同样等下一周期，不进入测试pending。
 * B1发送前再次检查ON，防止处理命令期间掉电；OFF刷新计时，不补发旧心跳。
 */
static void process_heartbeat(TickType_t *last_tick)
{
    TickType_t now = xTaskGetTickCount();
#if defined(B1_RK_UART)
    if (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) {
        *last_tick = now;
        return;
    }
#endif
    if ((TickType_t)(now - *last_tick) < pdMS_TO_TICKS(RK_HEARTBEAT_INTERVAL_MS)) return;
    //更新上次发送尝试的tick，准备发送心跳帧
    *last_tick = now;
    uint8_t wire[TLV_HEARTBEAT_FRAME_SIZE];
    uint64_t sequence = rk_link_next_sequence();
    int length = tlv_pack_heartbeat(sequence, wire, sizeof(wire));
    if (length == TLV_HEARTBEAT_FRAME_SIZE && uart_send(0, wire, (uint16_t)length) == 0) {
        rk_link_ready_note_tx(sequence);
        if (s_pending.waiting) s_pending.last_tx_sequence = sequence;
    }
}

/* argument未使用；每10ms推进一次，OFF也上报任务存活，不正常返回。 */
void StartRkTask(void *argument)
{
    (void)argument;
    // 初始化接收状态机和当前请求记录；请求/结果队列已由RTOS初始化代码创建。
    uint8_t buffer[UART_RX_BUF_SIZE];
    tlv_sm_t stream;
    tlv_sm_init(&stream);

    memset(&s_pending, 0, sizeof(s_pending));
    TickType_t last_heartbeat = xTaskGetTickCount();


    //循环读取
    for (;;) {
#if defined(B1_RK_UART)
        // 电源状态不是ON时重置接收状态、处理等待请求，每轮最多丢弃8个接收块。
        if (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON && !startup_rx_allowed()) {
            tlv_sm_reset(&stream);
            process_power_off();
            for (unsigned block = 0U; block < RK_RX_BATCH_MAX; ++block) {
                if (uart_poll_rx(0, buffer, sizeof(buffer)) == 0) break;
            }
            last_heartbeat = xTaskGetTickCount();
            task_watchdog_progress(TASK_WATCHDOG_RK);
            vTaskDelay(pdMS_TO_TICKS(20U));
            continue;
        }
#endif
        //每轮最多取8个RX块
        for (unsigned block = 0U; block < RK_RX_BATCH_MAX; ++block) {
            int length = uart_poll_rx(0, buffer, sizeof(buffer));
            if (length == 0) break;
            //数据不连续，重置状态机
            if (length == UART_RX_GAP) {
                tlv_sm_reset(&stream);
                continue;
            }
            //将接收到的字节逐个喂入状态机
            for (int i = 0; i < length; ++i) {
                if (tlv_sm_feed(&stream, buffer[i]) == 1) {
                    //状态机已接收完整帧，尝试解析并交付
                    tlv_frame_t frame;
                    if (tlv_sm_get_frame(&stream, &frame) < 0) {
                        // 解包失败时记录拒绝原因，继续处理当前接收块的下一个字节。
                        note_rx_reject(RK_RX_REJECT_TLV, NULL, 0U);
                        continue;
                    }
                    // 解包成功后交给accept_frame，继续检查心跳、Seq、算法和结果内容。
                    accept_frame(&frame);
                }
            }
        }
        //半帧是否超时
        (void)tlv_sm_check_timeout(&stream, RK_BYTE_TIMEOUT_MS);
        // 推进请求发送、等待时限和内部结果交付。
#if defined(B1_RK_UART)
        if (pwr_ctrl_get_state() == PWR_CTRL_STATE_ON) process_request();
        else process_power_off();
#else
        process_request();
#endif
        //到时间发送心跳
        process_heartbeat(&last_heartbeat);
        // 记录RK任务仍在推进，供独立看门狗检查使用；不要求RK实际在线。
        task_watchdog_progress(TASK_WATCHDOG_RK);
        //让出cpu
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}
