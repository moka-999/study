/* 正式C路由/组帧/窗口回归；队列与HAL为故障注入桩，不是物理串口或ADC验收。 */
#include "uart_router.h"
#include "uart_hw.h"
#include "idd_stats.h"
#include "state_machine.h"
#include "test_port.h"
#include <limits.h>
#include <string.h>

#define STR1(x) #x
#define STR(x) STR1(x)
#define CHECK(x) do { if (!(x)) fail("FAIL " STR(__LINE__) ": " #x "\n"); } while (0)
static uint32_t tick;
static int critical, isr;
static void message(const char *text)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = text;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}
static _Noreturn void finish(uint32_t code)
{
    const uint32_t block[] = {0x20026U, code};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register const uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) { }
}
static _Noreturn void fail(const char *text) { message(text); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }
void test_enter(void) { ++critical; }
void test_exit(void) { CHECK(critical > 0); --critical; }
TickType_t xTaskGetTickCount(void) { return tick; }

typedef struct {
    uint8_t blocks[8][UART_RX_BUF_SIZE + 2U];
    unsigned size, count, head, tail;
} fake_queue_t;
static fake_queue_t queues[2];
static unsigned queue_count;
static int allocation_failure;
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t size)
{
    CHECK(length == 8U && size <= sizeof(queues[0].blocks[0]) && queue_count < 2U);
    if (allocation_failure) return NULL;
    queues[queue_count].size = size;
    return &queues[queue_count++];
}
BaseType_t xQueueSendFromISR(QueueHandle_t handle, const void *data, BaseType_t *wake)
{
    fake_queue_t *q = handle;
    CHECK(isr && q && wake);
    if (q->count == 8U) return pdFALSE;
    memcpy(q->blocks[q->tail], data, q->size);
    q->tail = (q->tail + 1U) % 8U; ++q->count;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t handle, void *data, TickType_t wait)
{
    fake_queue_t *q = handle;
    CHECK(!isr && wait == 0U && critical);
    if (!q->count) return pdFALSE;
    memcpy(data, q->blocks[q->head], q->size);
    q->head = (q->head + 1U) % 8U; --q->count;
    return pdTRUE;
}
BaseType_t xQueueReset(QueueHandle_t handle)
{
    fake_queue_t *q = handle;
    CHECK(!isr && critical);
    q->count = q->head = q->tail = 0U;
    return pdTRUE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t handle) { return ((fake_queue_t *)handle)->count; }
int HAL_UARTEx_GetRxEventType(UART_HandleTypeDef *uart) { return uart->event; }
void uart_tx_mutex_init(void) { }
void uart_tx_isr_attach(void) { }
static UART_HandleTypeDef uart[2];
static uint8_t *dma_buffer[2];
static unsigned starts[2], aborts;
static int fail_start[2], fail_abort;
static int start_rx(uart_if_t *port, uint8_t *buffer, uint16_t length)
{
    unsigned index = port->handle == &uart[0] ? 0U : 1U;
    CHECK(length == UART_RX_BUF_SIZE);
    ++starts[index]; dma_buffer[index] = buffer;
    return fail_start[index] ? IF_ERR : IF_OK;
}
static int send_tx(uart_if_t *port, uint8_t *data, uint16_t length)
{ (void)port; (void)data; (void)length; return IF_OK; }
int uart_rx_abort(uart_if_t *port)
{
    CHECK(!isr && !critical && port->handle == &uart[0]);
    ++aborts;
    return fail_abort ? IF_ERR : IF_OK;
}
static uart_if_t ports[2] = {
    {.handle = &uart[0], .send = send_tx, .start_rx = start_rx},
    {.handle = &uart[1], .send = send_tx, .start_rx = start_rx},
};
static void receive(const uint8_t *bytes, uint16_t size, int event)
{
    CHECK(size <= UART_RX_BUF_SIZE && !critical);
    if (size) memcpy(dma_buffer[0], bytes, size);
    uart[0].event = event;
    isr = 1; HAL_UARTEx_RxEventCallback(&uart[0], size); isr = 0;
}
static void error_event(uint32_t error)
{
    uart[0].ErrorCode = error;
    isr = 1; HAL_UART_ErrorCallback(&uart[0]); isr = 0;
}
static int parse(tlv_sm_t *sm, const uint8_t *data, int length)
{
    int frames = 0;
    for (int i = 0; i < length; ++i) {
        if (tlv_sm_feed(sm, data[i]) == 1) {
            tlv_frame_t frame;
            CHECK(tlv_sm_get_frame(sm, &frame) >= 0);
            ++frames;
        }
    }
    return frames;
}

static void test_frames(void)
{
    uint8_t buffer[TLV_FRAME_MAX + 2U], data[TLV_CMD_DATA_MAX];
    memset(buffer, 0xCC, sizeof(buffer)); memset(data, 0x57, sizeof(data));
    CHECK(tlv_pack_cmd(1U, NULL, 1U, 0U, buffer, sizeof(buffer)) == 0);
    CHECK(tlv_pack_cmd(1U, (const uint8_t *)1, UINT16_MAX, 0U, buffer, UINT16_MAX) == 0);
    CHECK(tlv_pack_cmd(1U, data, sizeof(data) + 1U, 0U, buffer, sizeof(buffer)) == 0);
    CHECK(tlv_pack_cmd(1U, NULL, 0U, 0U, NULL, UINT16_MAX) == 0);
    CHECK(tlv_pack_heartbeat(0U, NULL, UINT16_MAX) == 0);
    CHECK(tlv_pack_heartbeat(0U, buffer, 19U) == 0);
    CHECK(tlv_pack_cmd(1U, data, sizeof(data), 0U, buffer, TLV_FRAME_MAX - 1U) == 0);
    for (unsigned i = 0U; i < sizeof(buffer); ++i) CHECK(buffer[i] == 0xCC);
    CHECK(tlv_pack_cmd(1U, data, sizeof(data), UINT64_MAX, buffer, TLV_FRAME_MAX) == TLV_FRAME_MAX);
    CHECK(buffer[TLV_FRAME_MAX] == 0xCC && buffer[TLV_FRAME_MAX + 1U] == 0xCC);
    tlv_sm_t sm; tlv_sm_init(&sm);
    CHECK(parse(&sm, buffer, TLV_FRAME_MAX) == 1);
    tlv_frame_t frame;
    CHECK(tlv_sm_get_frame(&sm, &frame) == 1 && frame.sequence == UINT64_MAX);
    CHECK(frame.units[0].data_len == sizeof(data) + 2U && !memcmp(frame.payload + 2, data, sizeof(data)));
    CHECK(tlv_pack_cmd(1U, NULL, 0U, 1U, buffer, sizeof(buffer)) == 22);
}

static void test_uart(void)
{
    uint8_t heartbeat[20], buffer[UART_RX_BUF_SIZE];
    static const uint8_t golden[20] = {0x5A,0,4,0x50,0x10,0,0,0,0,0,0,0,1,0,0,0,0,0x59,0xC2,0xA5};
    CHECK(tlv_pack_heartbeat(1U, heartbeat, sizeof(heartbeat)) == 20);
    CHECK(!memcmp(heartbeat, golden, sizeof(golden)));
    uart_rx_diagnostics_t diag;
    uart_register(-1, &ports[0]); uart_register(0, NULL);
    CHECK(!uart_get_rx_diagnostics(0, &diag));
    allocation_failure = 1; uart_register(0, &ports[0]);
    CHECK(!uart_get_rx_diagnostics(0, &diag));
    allocation_failure = 0; uart_register(0, &ports[0]); uart_register(0, &ports[0]);
    fail_start[1] = 1; uart_register(1, &ports[1]);
    uart_start_all(); CHECK(starts[0] == 1U && starts[1] == 1U);
    CHECK(uart_get_rx_diagnostics(1, &diag) && diag.restart_failures == 1U && diag.recovery_pending);
    receive(heartbeat, 10U, HAL_UART_RXEVENT_HT);
    CHECK(starts[0] == 1U && uart_poll_rx(0, buffer, sizeof(buffer)) == 0);
    receive(heartbeat, 20U, HAL_UART_RXEVENT_IDLE);
    CHECK(uart_poll_rx(0, buffer, 1U) == 0); /* 参数无效不得消费已有块。 */
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == 20 && !memcmp(buffer, golden, 20U));
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == 0);

    tlv_sm_t sm; tlv_sm_init(&sm);
    receive(heartbeat, 10U, HAL_UART_RXEVENT_IDLE);
    CHECK(parse(&sm, buffer, uart_poll_rx(0, buffer, sizeof(buffer))) == 0);
    for (unsigned i = 0U; i < 9U; ++i) receive(heartbeat, 20U, HAL_UART_RXEVENT_TC);
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == UART_RX_GAP);
    tlv_sm_reset(&sm); /* 对应RK任务的断流消费规则。 */
    CHECK(uart_get_rx_diagnostics(0, &diag) && diag.queue_overflows == 1U && diag.dropped_blocks == 9U);
    receive(heartbeat + 10U, 10U, HAL_UART_RXEVENT_IDLE);
    CHECK(parse(&sm, buffer, uart_poll_rx(0, buffer, sizeof(buffer))) == 0);
    receive(heartbeat, 20U, HAL_UART_RXEVENT_IDLE);
    CHECK(parse(&sm, buffer, uart_poll_rx(0, buffer, sizeof(buffer))) == 1);

    /* DMA满块也须完整交付，不能截断末尾或当作半传输事件重复交付。 */
    memset(buffer, 0xA7, sizeof(buffer));
    receive(buffer, sizeof(buffer), HAL_UART_RXEVENT_TC);
    memset(buffer, 0, sizeof(buffer));
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == UART_RX_BUF_SIZE);
    for (unsigned i = 0U; i < sizeof(buffer); ++i) CHECK(buffer[i] == 0xA7);
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == 0);

    fail_start[0] = 1; tick = 50U;
    receive(heartbeat, 20U, HAL_UART_RXEVENT_IDLE);
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == UART_RX_GAP && aborts == 0U);
    tick = 100U; CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == UART_RX_GAP && aborts == 1U);
    tick = 199U; CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == 0 && aborts == 1U);
    fail_start[0] = 0; fail_abort = 1; tick = 200U;
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == 0 && aborts == 2U);
    fail_abort = 0; tick = 300U;
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == 0 && aborts == 3U);
    CHECK(uart_get_rx_diagnostics(0, &diag) && diag.restart_failures == 2U &&
          diag.recovery_failures == 1U && diag.recoveries == 1U && !diag.recovery_pending);
    error_event(0x0CU); receive(heartbeat, 20U, HAL_UART_RXEVENT_IDLE);
    tick = 400U; CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == UART_RX_GAP);
    CHECK(uart_get_rx_diagnostics(0, &diag) && diag.errors == 1U && diag.last_hal_error == 0x0CU);
    receive(heartbeat, 20U, HAL_UART_RXEVENT_IDLE);
    tlv_sm_reset(&sm); CHECK(parse(&sm, buffer, uart_poll_rx(0, buffer, sizeof(buffer))) == 1);

    receive(NULL, 0U, HAL_UART_RXEVENT_IDLE);
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == UART_RX_GAP);
    CHECK(uart_get_rx_diagnostics(0, &diag) && diag.invalid_events == 1U);
    fail_start[0] = 1; tick = UINT32_MAX - 20U;
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == UART_RX_GAP);
    unsigned saved_aborts = aborts;
    tick = 10U; CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == 0 && aborts == saved_aborts);
    fail_start[0] = 0; tick = 79U;
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == 0 && aborts == saved_aborts + 1U);
    CHECK(uart_send(0, heartbeat, 20U) == IF_OK && uart_send(-1, heartbeat, 20U) == IF_ERR);
    CHECK(!critical && !isr);
}

static void test_stats(void)
{
    idd_stats_t stats = {0};
    memory_monitor_sample_t sample = {0};
    CHECK(idd_stats_begin(NULL, 0U, 10U, 1U) == IF_ERR);
    CHECK(idd_stats_begin(&stats, 0U, 0U, 1U) == IF_ERR);
    CHECK(idd_stats_begin(&stats, 0U, UINT32_MAX, 1U) == IF_ERR);
    CHECK(idd_stats_begin(&stats, 0U, 10U, 11U) == IF_ERR);
    CHECK(idd_stats_begin(&stats, 0U, 10U, 0U) == IF_ERR);
    CHECK(idd_stats_begin(&stats, 10U, 100U, 2U) == IF_OK);
    CHECK(idd_stats_begin(&stats, 20U, 200U, 1U) == IF_ERR && stats.started_ms == 10U);
    sample.valid_mask = MEMORY_MONITOR_VALID_VOLTAGE | MEMORY_MONITOR_VALID_IDD;
    for (unsigned i = 0U; i < 3U; ++i) {
        sample.voltage_mv[i] = 1000 + (int)i * 100; sample.idd[i].current_ma = i + 1U;
    }
    CHECK(idd_stats_feed(&stats, 10U, 11U, &sample) == 1);
    CHECK(idd_stats_feed(&stats, 10U, 11U, &sample) == 0); /* 重复不累计。 */
    CHECK(idd_stats_feed(&stats, 8U, 9U, &sample) == 0 && stats.state == IDD_STATS_RUNNING);
    for (unsigned i = 0U; i < 3U; ++i) { sample.voltage_mv[i] += 200; sample.idd[i].current_ma += 2U; }
    CHECK(idd_stats_feed(&stats, 20U, 21U, &sample) == 1);
    for (unsigned i = 0U; i < 3U; ++i) {
        CHECK(stats.voltage[i].mean == 1100 + (int)i * 100);
        CHECK(stats.voltage[i].minimum == 1000 + (int)i * 100);
        CHECK(stats.voltage[i].maximum == 1200 + (int)i * 100);
    }
    sample.valid_mask = MEMORY_MONITOR_VALID_IDD;
    CHECK(idd_stats_feed(&stats, 30U, 31U, &sample) == 1);
    sample.valid_mask = 0U;
    CHECK(idd_stats_feed(&stats, 40U, 41U, &sample) == 1);
    CHECK(stats.voltage[0].valid_count == 2U && stats.voltage[0].failed_count == 2U);
    CHECK(stats.current[0].valid_count == 3U && stats.current[0].failed_count == 1U);
    idd_stats_poll(&stats, 109U); CHECK(stats.state == IDD_STATS_RUNNING);
    idd_stats_poll(&stats, 110U); CHECK(stats.state == IDD_STATS_COMPLETE);
    idd_stats_t frozen = stats;
    CHECK(idd_stats_feed(&stats, 111U, 112U, &sample) == 0);
    idd_stats_cancel(&stats); CHECK(!memcmp(&stats, &frozen, sizeof(stats)));
    CHECK(idd_stats_begin(&stats, 0U, 10U, 1U) == IF_OK && stats.attempts == 0U);
    sample.valid_mask = MEMORY_MONITOR_VALID_VOLTAGE | MEMORY_MONITOR_VALID_IDD;
    CHECK(idd_stats_feed(&stats, 9U, 10U, &sample) == 0 && stats.state == IDD_STATS_TIMEOUT);
    CHECK(stats.attempts == 0U && stats.voltage[0].valid_count == 0U);
    CHECK(idd_stats_begin(&stats, UINT32_MAX - 10U, 30U, 1U) == IF_OK);
    CHECK(idd_stats_feed(&stats, UINT32_MAX - 9U, 4U, &sample) == 1);
    idd_stats_poll(&stats, 18U); CHECK(stats.state == IDD_STATS_RUNNING);
    idd_stats_poll(&stats, 19U); CHECK(stats.state == IDD_STATS_COMPLETE);
    CHECK(idd_stats_begin(&stats, 0U, INT32_MAX, 1U) == IF_OK);
    sample.voltage_mv[0] = INT32_MIN; sample.idd[0].current_ma = UINT32_MAX;
    CHECK(idd_stats_feed(&stats, 0U, 0U, &sample) == 1);
    sample.voltage_mv[0] = INT32_MAX;
    CHECK(idd_stats_feed(&stats, 1U, 1U, &sample) == 1);
    CHECK(stats.voltage[0].sum == -1 && stats.voltage[0].mean == 0);
    CHECK(stats.current[0].sum == (int64_t)UINT32_MAX * 2 && stats.current[0].mean == UINT32_MAX);
    idd_stats_cancel(&stats); CHECK(stats.state == IDD_STATS_CANCELLED && stats.attempts == 2U);
    CHECK(idd_stats_begin(&stats, 0U, 10U, 1U) == IF_OK);
    sample.valid_mask = MEMORY_MONITOR_VALID_VOLTAGE;
    CHECK(idd_stats_feed(&stats, 1U, 2U, &sample) == 1);
    idd_stats_poll(&stats, 10U); CHECK(stats.state == IDD_STATS_TIMEOUT);
    CHECK(stats.current[0].valid_count == 0U && stats.current[0].failed_count == 1U);
    /* RK完成事件可提前冻结窗口；样本不足仍不能伪装成完整测量。 */
    CHECK(idd_stats_begin(&stats, 0U, 1000U, 1U) == IF_OK);
    idd_stats_finish(&stats); CHECK(stats.state == IDD_STATS_TIMEOUT);
    CHECK(idd_stats_begin(&stats, 0U, 1000U, 1U) == IF_OK);
    sample.valid_mask = MEMORY_MONITOR_VALID_VOLTAGE | MEMORY_MONITOR_VALID_IDD;
    CHECK(idd_stats_feed(&stats, 1U, 2U, &sample) == 1);
    idd_stats_finish(&stats); CHECK(stats.state == IDD_STATS_COMPLETE);
    frozen = stats;
    idd_stats_finish(&stats); idd_stats_poll(&stats, 2000U);
    CHECK(!memcmp(&stats, &frozen, sizeof(stats)));
    idd_stats_finish(NULL);
}

void test_main(void)
{
    test_frames(); test_uart(); test_stats();
    message("PASS: UART recovery, RK bounds and IDD windows\n");
    finish(0U);
}
