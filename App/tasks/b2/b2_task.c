#include "b2_task.h"
#include "b2_state_machine.h"
#include "b2_service.h"
#include "b2_spi.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include <string.h>

static b2_link_diagnostics_t s_diagnostics;
/* 固定缓冲不占任务栈，不进行每帧malloc；只保留上一请求及应答，不做历史重放表。 */
static b2_state_machine_t s_parser;
static b2_spi_packet_t s_packet;
static uint8_t s_previous[B2_MAX_FRAME];
static size_t s_previous_length;
static uint64_t s_previous_sequence;
static uint8_t s_response[B2_RESPONSE_SIZE];
static size_t s_response_length;

static void handle_packet(void)
{
    b2_request_t request;
    b2_sm_begin(&s_parser);
    for (size_t i = 0U; i < s_packet.length; ++i) b2_sm_feed(&s_parser, s_packet.data[i]);
    if (!b2_sm_end(&s_parser, &request)) {
        taskENTER_CRITICAL(); ++s_diagnostics.invalid_frames; taskEXIT_CRITICAL();
        return;
    }
    taskENTER_CRITICAL();
    ++s_diagnostics.valid_requests;
    s_diagnostics.last_valid_ms = HAL_GetTick();
    s_diagnostics.online = 1U;
    taskEXIT_CRITICAL();
    if (s_previous_length && request.sequence == s_previous_sequence) {
        if (s_previous_length == s_packet.length &&
            memcmp(s_previous, s_packet.data, s_packet.length) == 0) {
            taskENTER_CRITICAL(); ++s_diagnostics.duplicate_requests; taskEXIT_CRITICAL();
            (void)b2_spi_publish(s_packet.token, s_response, s_response_length);
        } else {
            /* 同Seq换内容不是重试：拒绝且保留上一份缓存，不重新执行副作用。 */
            uint8_t error[B2_RESPONSE_SIZE];
            size_t length = b2_response_build(&request, B2_STATUS_INVALID, 0, 0, error, sizeof(error));
            (void)b2_spi_publish(s_packet.token, error, length);
        }
        return;
    }
    b2_service_reply_t reply;
    b2_service_exchange(&request, &reply);
    s_response_length = b2_response_build(&request, reply.status, reply.tester_id,
                                         reply.site_id, s_response, sizeof(s_response));
    memcpy(s_previous, s_packet.data, s_packet.length);
    s_previous_length = s_packet.length;
    s_previous_sequence = request.sequence;
    (void)b2_spi_publish(s_packet.token, s_response, s_response_length);
}

void StartB2Task(void *argument)
{
    (void)argument;
    if (!b2_spi_start()) { vTaskDelete(NULL); return; }
    for (;;) {
        if (b2_spi_receive(&s_packet)) handle_packet();
        taskENTER_CRITICAL();
        if ((uint32_t)(HAL_GetTick() - s_diagnostics.last_valid_ms) >= 3000U)
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
    if ((uint32_t)(HAL_GetTick() - out->last_valid_ms) >= 3000U) out->online = 0U;
    taskEXIT_CRITICAL();
}
