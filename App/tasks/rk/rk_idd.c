/* IDD正式采集适配；不改变供电，不判断电流限值，不在中断内扫描逐点缓冲。 */
#include "rk_idd.h"
#include "idd_session.h"
#include "monitor_task.h"
#include "uart_router.h"
#include "semphr.h"
#include "main.h"
#include <string.h>
#if defined(B1_RK_UART)
#include "rk_trace.h"
#endif

/* 115200下2047字节约178ms；1s仍无TC即视为发送状态未知，不自动重发。 */
#define IDD_TX_TIMEOUT_MS 1000U
/* 监测生产者正常每轮推进；停滞超过1s则取消本次窗口，不能报默认PASS。 */
#define IDD_MONITOR_TIMEOUT_MS 1000U

_Static_assert(MEMORY_MONITOR_RAIL_COUNT == IDD_SESSION_RAILS, "采样与IDD电流轨数量必须一致");

static idd_session_point_t s_points[IDD_SESSION_CAPACITY]
    __attribute__((section(".idd_capture"), aligned(4)));
static idd_session_t s_session;
static idd_stats_t s_measurement;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static uint32_t s_ticket, s_tx_started_ms, s_last_sample_ms, s_sample_sequence;
static uint64_t s_tx_sequence;

/* 持会话锁调用；实际停止位时间由UART中断记录，任务晚醒不会推迟窗口起点。 */
static void process_tx(uint32_t now_ms)
{
    if (s_session.phase != IDD_SESSION_ENTER_TX && s_session.phase != IDD_SESSION_EXIT_TX) return;
    uint32_t completed_ms;
    int status = uart_tx_status(UART_PORT_RK_PROTOCOL, s_ticket, &completed_ms);
    if (status == 1) {
        int entering = s_session.phase == IDD_SESSION_ENTER_TX;
        (void)idd_session_tx_complete(&s_session, s_tx_sequence, completed_ms);
        if (entering) {
            (void)idd_stats_begin(&s_measurement, s_session.result.started_ms,
                                  IDD_SESSION_WINDOW_MS, 1U);
        }
    } else if (status < 0 || (uint32_t)(now_ms - s_tx_started_ms) >= IDD_TX_TIMEOUT_MS) {
        idd_session_tx_failed(&s_session, s_tx_sequence, now_ms);
    }
}

/* 此回调同步消费实际新读取；只在任务互斥锁内冻结80KiB缓冲，允许中断抢占。 */
static void sample_observer(const memory_monitor_sample_t *sample, uint8_t groups,
                             uint32_t begin_ms, uint32_t end_ms)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;
    uint32_t now_ms = HAL_GetTick();
    s_last_sample_ms = now_ms;
    process_tx(now_ms);
    if (sample != NULL && (groups & MEMORY_MONITOR_VALID_IDD) != 0U) {
        uint32_t current[IDD_SESSION_RAILS] = {0};
        uint8_t valid = 0U;
        ++s_sample_sequence;
        for (unsigned rail = 0U; rail < IDD_SESSION_RAILS; ++rail) {
            if ((sample->valid_mask & MEMORY_MONITOR_VALID_IDD) != 0U &&
                sample->idd[rail].current_ua <= UINT32_MAX) {
                current[rail] = (uint32_t)sample->idd[rail].current_ua;
                valid |= (uint8_t)(1U << rail);
            }
        }
        (void)idd_session_feed(&s_session, s_sample_sequence, begin_ms, end_ms, current, valid);
    }
    if (sample != NULL && (groups & MEMORY_MONITOR_VALID_VOLTAGE) != 0U) {
        (void)idd_stats_feed_groups(&s_measurement, begin_ms, end_ms, sample,
                                    MEMORY_MONITOR_VALID_VOLTAGE);
    }
    /* 只有生产者的轮末事件推进截止点，避免RK任务抢先冻结尚未提交的在途读取。 */
    if (sample == NULL) idd_session_poll(&s_session, end_ms);
    if (s_session.frozen) idd_stats_finish(&s_measurement);
    xSemaphoreGive(s_mutex);
}

void rk_idd_init(void)
{
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    if (s_mutex != NULL) monitor_set_sample_observer(sample_observer);
}

static int send_frame(uint64_t sequence)
{
    uint8_t wire[TLV_IDD_ACTION_FRAME_SIZE];
    int length = idd_session_next_frame(&s_session, sequence, wire, sizeof(wire));
    int queued = length == TLV_IDD_ACTION_FRAME_SIZE &&
        uart_send_tracked(UART_PORT_RK_PROTOCOL, wire, (uint16_t)length, &s_ticket) == IF_OK;
    s_tx_sequence = sequence;
    s_tx_started_ms = HAL_GetTick();
#if defined(B1_RK_UART)
    if (queued) rk_link_note_queued(sequence);
    rk_trace_sent(s_session.result.command, wire, length, queued);
#endif
    return queued;
}

int rk_idd_start(uint16_t command, uint64_t sequence)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return -1;
    int result = -1;
    if (idd_session_begin(&s_session, command, s_points, IDD_SESSION_CAPACITY) == 0) {
        idd_stats_reset(&s_measurement);
        s_last_sample_ms = HAL_GetTick();
        if (send_frame(sequence)) result = 0;
        else {
            /* 整帧入队失败保证零字节发送，此时无需退出，也不能保留虚假的在途。 */
            memset(&s_session, 0, sizeof(s_session));
        }
    }
    xSemaphoreGive(s_mutex);
    return result;
}

int rk_idd_process(rk_response_t *response)
{
    if (response == NULL || s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return 0;
    uint32_t now_ms = HAL_GetTick();
    process_tx(now_ms);
    if ((s_session.phase == IDD_SESSION_SETTLING || s_session.phase == IDD_SESSION_CAPTURING) &&
        (uint32_t)(now_ms - s_last_sample_ms) > IDD_MONITOR_TIMEOUT_MS) {
        idd_session_cancel(&s_session, now_ms);
    }
    if (s_session.phase == IDD_SESSION_EXIT_READY && !send_frame(rk_link_next_sequence())) {
        /* 进入已发出而退出未成功发送，RK状态未知，保持占用到确认掉电。 */
        idd_session_tx_failed(&s_session, s_tx_sequence, HAL_GetTick());
    }
    idd_session_result_t result;
    int ready = idd_session_take_result(&s_session, &result);
    if (ready) {
#if defined(B1_RK_UART)
        rk_trace_idd_result(&result);
#endif
        idd_stats_finish(&s_measurement);
        /* 电流严格从保存的全部新样点计算，不能用按毫秒去重的FUNC窗口替代。 */
        for (unsigned rail = 0U; rail < IDD_SESSION_RAILS; ++rail) {
            const idd_session_metric_t *source = &result.current[rail];
            idd_metric_stats_t *dest = &s_measurement.current[rail];
            dest->sum = (int64_t)source->sum_ua;
            dest->mean = source->mean_ua;
            dest->minimum = source->minimum_ua;
            dest->maximum = source->maximum_ua;
            dest->valid_count = source->valid_count;
            dest->failed_count = source->failed_count;
        }
        /* 电流回填后重算六组样本是否齐全；会话质量错误另由response状态表达。 */
        if (s_measurement.minimum_samples != 0U) {
            s_measurement.state = IDD_STATS_RUNNING;
            idd_stats_finish(&s_measurement);
        }
        memset(response, 0, sizeof(*response));
        response->status = (result.flags & IDD_SESSION_TX_ERROR) != 0U ? RK_RESPONSE_TIMEOUT :
                           result.flags != 0U ? RK_RESPONSE_MEASUREMENT_FAILED : RK_RESPONSE_COMPLETE;
        response->result.command_id = result.command;
        response->result.elapsed_ms = (uint32_t)(now_ms - result.started_ms);
        response->measurement = s_measurement;
    }
    xSemaphoreGive(s_mutex);
    return ready;
}

void rk_idd_power_off(void)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;
    idd_session_power_off(&s_session, HAL_GetTick());
    idd_session_result_t discarded;
    (void)idd_session_take_result(&s_session, &discarded);
    idd_stats_cancel(&s_measurement);
    xSemaphoreGive(s_mutex);
}

void rk_idd_cancel(void)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;
    idd_session_cancel(&s_session, HAL_GetTick());
    xSemaphoreGive(s_mutex);
}
