/* RK串口任务：收发零TLV心跳、缓存参考进度、汇总详情与FAIL后一次交付。
 * 结果超时后RK执行状态未知，暂停后续命令；仅匹配的迟到结果或B1断电解除。
 * UART/DMA由既有路由层负责；无PC JSON、供电时序或测试算法实现。
 */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "uart_router.h"
#include "state_machine.h"
#include "rk_link.h"
#include "ota_app.h"
#if defined(B1_RK_UART)
#include "pwr_ctrl.h"
#endif
#include <string.h>

/* MCU向RK发送心跳的间隔：1000ms（1秒）；不是算法超时或RK-ready判据。 */
#define RK_HEARTBEAT_INTERVAL_MS 1000U
/* 每轮最多取8个RX块（含断流通知）；持续来数据也必须让出CPU并推进心跳/超时。
 * 未取走的块留在UART队列，半帧保留在状态机中，不因达到配额而清空。
 */
#define RK_RX_BATCH_MAX 8U

typedef struct {
    rk_request_t request;
    rk_response_t response;
    rk_test_result_t result; /* 已收到的详情与FAIL；result_fields标明哪些部分有效。 */
    uint64_t sequence;        /* 本次下行测试命令Seq；仅作为接收窗口起点和内部诊断。 */
    uint64_t last_tx_sequence; /* 本次等待期间最近成功入UART队列的MCU Seq。 */
    uint64_t progress_sequence; /* RK进度携带的最近已收MCU Seq，仅作参考。 */
    TickType_t sent_tick;
    uint8_t waiting;       /* 正等待该Seq/命令的最终结果，含超时后的状态未知期。 */
    uint8_t timed_out;     /* 已判定TIMEOUT，可能仍待入队；不得再交付COMPLETE。 */
    uint8_t delivery;      /* 已完成但结果队列满时，保留副本直到交付。 */
    uint8_t recovered;     /* 迟到结果/断电已解除未知；须等TIMEOUT入队后再报恢复。 */
    uint8_t result_fields; /* RK_REPORT_DETAILS/FAIL，必须收齐两部分才可完成。 */
    uint8_t result_conflict; /* 当前测试的重复结果内容矛盾；保持等待，不猜哪份正确。 */
    uint8_t progress_valid; /* 新请求清零；1表示本次等待中收到过合法进度心跳。 */
    uint8_t progress_percent; /* 0..100，仅参考，不代表DDR-ready或测试结束。 */
} rk_pending_t;
static rk_pending_t s_pending;

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
    if (s_pending.delivery) {
        if (xQueueSend(rkResponseQueue, &s_pending.response, 0U) != pdTRUE) return;
        s_pending.delivery = 0U;
    }
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
 * uint64无符号差支持计数回绕；两包无需同Seq，按单在途及详情算法号汇总。
 * 同算法的旧结果若被RK重新赋予当前窗口Seq，协议无法区分，不能据Seq宣称防重放。
 * 进度只保留观察值，不生成PC新字段；结果内容冲突不放行后续测试。
 */
static void accept_frame(const tlv_frame_t *frame)
{
    rk_report_t report;
    if (!s_pending.waiting || frame == NULL ||
        (uint64_t)(frame->sequence - s_pending.sequence) >
        (uint64_t)(s_pending.last_tx_sequence - s_pending.sequence) ||
        rk_report_decode(frame, &report) != 0) return;
    const uint8_t complete = RK_REPORT_DETAILS | RK_REPORT_FAIL;
    if ((report.fields & RK_REPORT_PROGRESS) && !(report.fields & complete)) {
        s_pending.progress_percent = report.progress_percent;
        s_pending.progress_sequence = frame->sequence;
        s_pending.progress_valid = 1U;
        return;
    }
    if (s_pending.result_conflict || !(report.fields & complete)) return;
    if (report.fields & RK_REPORT_DETAILS) {
        if (report.result.command_id != s_pending.request.command_id) return;
        if ((s_pending.result_fields & RK_REPORT_DETAILS) &&
            (report.result.error_address != s_pending.result.error_address ||
             report.result.die != s_pending.result.die ||
             report.result.elapsed_ms != s_pending.result.elapsed_ms)) {
            s_pending.result_conflict = 1U;
            return;
        }
    }
    if ((report.fields & RK_REPORT_FAIL) && (s_pending.result_fields & RK_REPORT_FAIL) &&
        report.result.failed != s_pending.result.failed) {
        s_pending.result_conflict = 1U;
        return;
    }
    /* 全部检查完成后再更新缓存，坏帧/矛盾帧不提交其中某个有效字段。 */
    if (report.fields & RK_REPORT_DETAILS) {
        uint8_t failed = s_pending.result.failed;
        s_pending.result = report.result;
        s_pending.result.failed = failed;
    }
    if (report.fields & RK_REPORT_FAIL) s_pending.result.failed = report.result.failed;
    s_pending.result_fields |= report.fields & complete;
    if (s_pending.result_fields != complete) return;
    if (result_wait_expired()) {
        s_pending.timed_out = 1U;
        finish_request(RK_RESPONSE_TIMEOUT);
    }
    s_pending.waiting = 0U;
    /* 已向业务层判超时的请求不能改成第二次成功；迟到结果仅证明RK已结束。 */
    if (s_pending.timed_out) {
        s_pending.recovered = 1U;
        return;
    }
    finish_request(RK_RESPONSE_COMPLETE);
    s_pending.response.result = s_pending.result;
}
/* 每轮调用；只处理一个请求，UART整帧入队失败不会丢请求或无界重试。 */
static void process_request(void)
{
    deliver_result();
    if (s_pending.waiting) {
        if (result_wait_expired()) {
            s_pending.timed_out = 1U;
            finish_request(RK_RESPONSE_TIMEOUT);
        }
        return;
    }
    if (s_pending.delivery || s_pending.recovered ||
        rkRequestQueue == NULL || rkResponseQueue == NULL) return;
    rk_request_t request;
    if (xQueueReceive(rkRequestQueue, &request, 0U) != pdTRUE) return;
    memset(&s_pending, 0, sizeof(s_pending));
    s_pending.request = request;
    uint64_t ticks = (uint64_t)request.timeout_ms * configTICK_RATE_HZ / 1000U;
    if (rk_command_find_by_id(request.command_id) == NULL ||
        (request.timeout_ms != 0U && (ticks == 0U || ticks > INT32_MAX))) {
        finish_request(RK_RESPONSE_INVALID);
        return;
    }
    s_pending.sequence = rk_link_next_sequence();
    uint8_t wire[TLV_CMD_FRAME_SIZE];
    int length = tlv_pack_cmd(request.command_id, NULL, 0U,
                              s_pending.sequence, wire, sizeof(wire));
    if (length != TLV_CMD_FRAME_SIZE || uart_send(0, wire, (uint16_t)length) != 0) {
        /* uart_send失败保证未向TX环写入任何字节；不回插队列冒丢失风险。 */
        finish_request(RK_RESPONSE_TX_FAILED);
        return;
    }
    s_pending.sent_tick = xTaskGetTickCount();
    s_pending.last_tx_sequence = s_pending.sequence;
    s_pending.waiting = 1U;
}
#if defined(B1_RK_UART)
/* B1掉电不静默清结果；未执行请求逐条返回POWER_OFF，不假装完成测试。 */
static void process_power_off(void)
{
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
    *last_tick = now;
    uint8_t wire[TLV_HEARTBEAT_FRAME_SIZE];
    uint64_t sequence = rk_link_next_sequence();
    int length = tlv_pack_heartbeat(sequence, wire, sizeof(wire));
    if (length == TLV_HEARTBEAT_FRAME_SIZE && uart_send(0, wire, (uint16_t)length) == 0 &&
        s_pending.waiting) {
        s_pending.last_tx_sequence = sequence;
    }
}

/* argument未使用；每10ms推进一次，OFF也上报OTA任务健康，不正常返回。 */
void StartRkTask(void *argument)
{
    (void)argument;
    uint8_t buffer[UART_RX_BUF_SIZE];
    tlv_sm_t stream;
    tlv_sm_init(&stream);
    memset(&s_pending, 0, sizeof(s_pending));
    TickType_t last_heartbeat = xTaskGetTickCount();
    for (;;) {
#if defined(B1_RK_UART)
        if (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) {
            tlv_sm_reset(&stream);
            process_power_off();
            for (unsigned block = 0U; block < RK_RX_BATCH_MAX; ++block) {
                if (uart_poll_rx(0, buffer, sizeof(buffer)) == 0) break;
            }
            last_heartbeat = xTaskGetTickCount();
            ota_app_progress(OTA_TASK_RK);
            vTaskDelay(pdMS_TO_TICKS(20U));
            continue;
        }
#endif
        for (unsigned block = 0U; block < RK_RX_BATCH_MAX; ++block) {
            int length = uart_poll_rx(0, buffer, sizeof(buffer));
            if (length == 0) break;
            if (length == UART_RX_GAP) {
                tlv_sm_reset(&stream);
                continue;
            }
            for (int i = 0; i < length; ++i) {
                if (tlv_sm_feed(&stream, buffer[i]) == 1) {
                    tlv_frame_t frame;
                    if (tlv_sm_get_frame(&stream, &frame) < 0) continue;
                    accept_frame(&frame);
                }
            }
        }
        (void)tlv_sm_check_timeout(&stream, RK_BYTE_TIMEOUT_MS);
        process_request();
        process_heartbeat(&last_heartbeat);
        ota_app_progress(OTA_TASK_RK);
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}
