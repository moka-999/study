#include "b2_task.h"
#include "b2_state_machine.h"
#include "b2_service.h"
#include "b2_spi.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include <string.h>

#define B2_LINK_TIMEOUT_MS 3000U

static b2_link_diagnostics_t s_diagnostics;
/* 固定缓冲不占任务栈，不进行每帧malloc；只保留上一请求及应答，不做历史重放表。 */
static b2_state_machine_t s_parser;
static b2_spi_packet_t s_packet;
static uint8_t s_previous[B2_MAX_FRAME];
static size_t s_previous_length;
static uint64_t s_previous_sequence;
static uint8_t s_response[B2_RESPONSE_SIZE];
static size_t s_response_length;

static int link_expired(uint32_t now_ms, uint32_t last_ms)
{
    return (uint32_t)(now_ms - last_ms) >= B2_LINK_TIMEOUT_MS;
}

static void handle_packet(void)
{

    b2_request_t request;
    //重置状态机
    b2_sm_begin(&s_parser);
    //将收到的每个字节喂入状态机
    for (size_t i = 0U; i < s_packet.length; ++i) b2_sm_feed(&s_parser, s_packet.data[i]);
    //状态机结束后尝试解码请求，如果失败则增加诊断计数器并返回
    if (!b2_sm_end(&s_parser, &request)) {
        taskENTER_CRITICAL(); ++s_diagnostics.invalid_frames; taskEXIT_CRITICAL();
        return;
    }
    taskENTER_CRITICAL();
    uint32_t now = HAL_GetTick();
    if (!s_diagnostics.online || link_expired(now, s_diagnostics.last_valid_ms)) {
        ++s_diagnostics.session;
        /* B2重新上线可从原序号开始，上一会话的应答不能代替本次状态更新。 */
        s_previous_length = 0U;
    }
    ++s_diagnostics.valid_requests;
    s_diagnostics.last_valid_ms = now;
    s_diagnostics.online = 1U;
    taskEXIT_CRITICAL();
    //如果收到的请求与上一请求相同，则直接返回上一应答；否则调用业务处理函数
    if (s_previous_length && request.sequence == s_previous_sequence) {
        if (s_previous_length == s_packet.length &&
            memcmp(s_previous, s_packet.data, s_packet.length) == 0) {
            taskENTER_CRITICAL(); ++s_diagnostics.duplicate_requests; taskEXIT_CRITICAL();
            //同一请求的重复帧直接返回上一应答，避免副作用重复执行
            (void)b2_spi_publish(s_packet.token, s_response, s_response_length);
        } else {
            /* 同Seq换内容不是重试：拒绝且保留上一份缓存，不重新执行副作用。 */
            uint8_t error[B2_RESPONSE_SIZE];
            size_t length = b2_response_build(&request, B2_STATUS_INVALID, 0, 0, error, sizeof(error));
            (void)b2_spi_publish(s_packet.token, error, length);
        }
        return;
    }
    //新请求，调用业务处理函数
    b2_service_reply_t reply;
    b2_service_exchange(&request, &reply);
    s_response_length = b2_response_build(&request, reply.status, reply.tester_id,
                                         reply.site_id, s_response, sizeof(s_response));
    memcpy(s_previous, s_packet.data, s_packet.length);
    s_previous_length = s_packet.length;
    s_previous_sequence = request.sequence;
    //将应答发布到SPI链路，等待主机读取
    (void)b2_spi_publish(s_packet.token, s_response, s_response_length);
}

void StartB2Task(void *argument)
{
    (void)argument;
    if (!b2_spi_start()) { vTaskDelete(NULL); return; }
    for (;;) {
        if (b2_spi_receive(&s_packet)) handle_packet();
        taskENTER_CRITICAL();
        // 3秒没有结构完整、命令可识别的帧则离线
        if (link_expired(HAL_GetTick(), s_diagnostics.last_valid_ms))
            s_diagnostics.online = 0U;
        taskEXIT_CRITICAL();
        /* IRQ已收存CS快照；任务不忙轮询SPI寄存器，1ms让出CPU给RK与采样。 */
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}

void b2_link_diagnostics(b2_link_diagnostics_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL();
    *out = s_diagnostics;
    /* 业务应答可能仍在排队，读取时也按时间计算，不能把阻塞前的在线位当作实时值。 */
    if (link_expired(HAL_GetTick(), out->last_valid_ms)) out->online = 0U;
    taskEXIT_CRITICAL();
}
