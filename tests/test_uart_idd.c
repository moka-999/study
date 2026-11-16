/* 正式C路由/组帧/窗口回归；队列与HAL为故障注入桩，不是物理串口或ADC验收。 */
#include "uart_router.h"
#include "uart_hw.h"
#include "idd_stats.h"
#include "state_machine.h"
#include "test_port.h"
#include <limits.h>
#include <string.h>
#if defined(TEST_B1_SERIAL_NET)
#include "serial_net.h"
#endif
#if defined(TEST_B1_LOG_CONSOLE)
#include "log_console.h"
#include "result_led.h"
#endif

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
uint32_t __get_IPSR(void) { return (uint32_t)isr; }

typedef struct {
    uint8_t blocks[8][UART_RX_BUF_SIZE + 2U];
    unsigned size, capacity, count, head, tail;
} fake_queue_t;
static fake_queue_t queues[3];
static unsigned queue_count;
static int allocation_failure;
static void (*queue_wait_event)(void);
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t size)
{
    CHECK((length == 8U || length == 2U) && size <= sizeof(queues[0].blocks[0]) && queue_count < 3U);
    if (allocation_failure) return NULL;
    queues[queue_count].size = size;
    queues[queue_count].capacity = length;
    return &queues[queue_count++];
}
BaseType_t xQueueSendFromISR(QueueHandle_t handle, const void *data, BaseType_t *wake)
{
    fake_queue_t *q = handle;
    CHECK(isr && q && wake);
    if (q->count == q->capacity) return pdFALSE;
    memcpy(q->blocks[q->tail], data, q->size);
    q->tail = (q->tail + 1U) % q->capacity; ++q->count;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t handle, void *data, TickType_t wait)
{
    fake_queue_t *q = handle;
    CHECK(!isr && wait == 0U && critical);
    if (!q->count) return pdFALSE;
    memcpy(data, q->blocks[q->head], q->size);
    q->head = (q->head + 1U) % q->capacity; --q->count;
    return pdTRUE;
}
BaseType_t xQueuePeek(QueueHandle_t handle, void *data, TickType_t wait)
{
    fake_queue_t *q = handle;
    CHECK(!isr && !critical);
    if (!q->count && wait) {
        if (queue_wait_event) {
            void (*event)(void) = queue_wait_event;
            queue_wait_event = NULL;
            event();
        } else tick += wait;
    }
    if (!q->count) return pdFALSE;
    memcpy(data, q->blocks[q->head], q->size);
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
static UART_HandleTypeDef uart[3];
static uint8_t *dma_buffer[3];
static unsigned starts[3], aborts;
static int fail_start[3], fail_abort;
#if defined(TEST_B1_LOG_CONSOLE)
/* 用输出位代替灯珠；链接正式result_led.c，验证命令确实产生对应GPIO和相位。 */
static GPIO_TypeDef led_gpio;
uint32_t HAL_GetTick(void) { return tick; }
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState level)
{
    CHECK(port == &led_gpio && pin != 0U && (pin & ~0x3FU) == 0U);
    if (level == GPIO_PIN_SET) port->unused |= pin;
    else port->unused &= ~(uint32_t)pin;
}
static char console_output[8192];
static size_t console_output_length;
static int fail_console_send;
static unsigned console_send_count;
static uint8_t rk_console_output[8192];
static size_t rk_console_output_length;
static int fail_rk_console_send;
static unsigned protocol_send_count;
#endif
static int start_rx(uart_if_t *port, uint8_t *buffer, uint16_t length)
{
    unsigned index = (unsigned)((UART_HandleTypeDef *)port->handle - uart);
    CHECK(index < 3U);
    CHECK(length == UART_RX_BUF_SIZE);
    ++starts[index]; dma_buffer[index] = buffer;
    return fail_start[index] ? IF_ERR : IF_OK;
}
static int send_tx(uart_if_t *port, uint8_t *data, uint16_t length)
{
#if defined(TEST_B1_LOG_CONSOLE)
    if (port->handle == &uart[0]) ++protocol_send_count;
    if (port->handle == &uart[1]) {
        CHECK(!isr);
        if (fail_rk_console_send) return IF_ERR;
        CHECK(rk_console_output_length + length <= sizeof(rk_console_output));
        memcpy(rk_console_output + rk_console_output_length, data, length);
        rk_console_output_length += length;
    }
    if (port->handle == &uart[2]) {
        CHECK(!isr && critical && length <= LOG_CONSOLE_RECORD_SIZE);
        ++console_send_count;
        if (fail_console_send) return IF_ERR;
        CHECK(console_output_length + length < sizeof(console_output));
        memcpy(console_output + console_output_length, data, length);
        console_output_length += length;
        console_output[console_output_length] = '\0';
    }
#else
    (void)port; (void)data; (void)length;
#endif
    return IF_OK;
}
int uart_rx_abort(uart_if_t *port)
{
    CHECK(!isr && !critical &&
          (port->handle == &uart[0] || port->handle == &uart[1] || port->handle == &uart[2]));
    ++aborts;
    return fail_abort ? IF_ERR : IF_OK;
}
static uart_if_t ports[3] = {
    {.handle = &uart[0], .send = send_tx, .start_rx = start_rx},
    {.handle = &uart[1], .send = send_tx, .start_rx = start_rx},
    {.handle = &uart[2], .send = send_tx, .start_rx = start_rx},
};
static void receive_port(unsigned port, const uint8_t *bytes, uint16_t size, int event)
{
    CHECK(size <= UART_RX_BUF_SIZE && !critical);
    CHECK(port < 3U && dma_buffer[port]);
    if (size) memcpy(dma_buffer[port], bytes, size);
    uart[port].event = event;
    isr = 1; HAL_UARTEx_RxEventCallback(&uart[port], size); isr = 0;
}
static void receive(const uint8_t *bytes, uint16_t size, int event)
{ receive_port(0U, bytes, size, event); }
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
    CHECK(tlv_pack_heartbeat(0U, buffer, 15U) == 0);
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

#if defined(TEST_B1_LOG_CONSOLE)
static void receive_while_waiting(void)
{
    static const uint8_t bytes[] = {0x12U, 0x34U, 0x56U};
    receive(bytes, sizeof(bytes), HAL_UART_RXEVENT_IDLE);
}

static void test_uart_wait(void)
{
    uint8_t buffer[UART_RX_BUF_SIZE];
    uint32_t before = tick;
    uart_wait_rx(-1, 1U);
    uart_wait_rx(UART_PORT_RK_PROTOCOL, 0U);
    CHECK(tick == before);
    uart_wait_rx(UART_PORT_RK_PROTOCOL, 1U);
    CHECK(tick == before + 1U);
    /* 等待时注入ISR入队；唤醒及反复peek均不得消费块或附加一轮固定等待。 */
    before = tick;
    queue_wait_event = receive_while_waiting;
    uart_wait_rx(UART_PORT_RK_PROTOCOL, 1U);
    CHECK(queue_wait_event == NULL && tick == before);
    uart_wait_rx(UART_PORT_RK_PROTOCOL, 1U);
    CHECK(tick == before);
    CHECK(uart_poll_rx(UART_PORT_RK_PROTOCOL, buffer, sizeof(buffer)) == 3);
    CHECK(buffer[0] == 0x12U && buffer[1] == 0x34U && buffer[2] == 0x56U);
    CHECK(uart_poll_rx(UART_PORT_RK_PROTOCOL, buffer, sizeof(buffer)) == 0);
    message("PASS: UART wait preserves RX data, idle timeout and ISR wake path\n");
}
#endif

static void test_uart(void)
{
    uint8_t heartbeat[16], buffer[UART_RX_BUF_SIZE];
    static const uint8_t golden[16] = {0x5A,0,0,0x51,0xD3,0,0,0,0,0,0,0,5,0x80,0x08,0xA5};
    CHECK(tlv_pack_heartbeat(5U, heartbeat, sizeof(heartbeat)) == 16);
    CHECK(!memcmp(heartbeat, golden, sizeof(golden)));
    uart_rx_diagnostics_t diag;
    uart_register(-1, &ports[0]); uart_register(0, NULL);
    CHECK(!uart_get_rx_diagnostics(0, &diag));
    allocation_failure = 1; uart_register(0, &ports[0]);
    CHECK(!uart_get_rx_diagnostics(0, &diag));
    allocation_failure = 0; uart_register(0, &ports[0]); uart_register(0, &ports[0]);
    fail_start[1] = 1; uart_register(1, &ports[1]);
#if defined(TEST_B1_LOG_CONSOLE)
    uart_register(UART_PORT_CONSOLE, &ports[2]);
#endif
    uart_start_all(); CHECK(starts[0] == 1U && starts[1] == 1U);
    CHECK(uart_get_rx_diagnostics(1, &diag) && diag.restart_failures == 1U && diag.recovery_pending);
    receive(heartbeat, 10U, HAL_UART_RXEVENT_HT);
    CHECK(starts[0] == 1U && uart_poll_rx(0, buffer, sizeof(buffer)) == 0);
    receive(heartbeat, 16U, HAL_UART_RXEVENT_IDLE);
    CHECK(uart_poll_rx(0, buffer, 1U) == 0); /* 参数无效不得消费已有块。 */
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == 16 && !memcmp(buffer, golden, 16U));
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == 0);

#if defined(TEST_B1_LOG_CONSOLE)
    test_uart_wait();
#endif
    tlv_sm_t sm; tlv_sm_init(&sm);
    receive(heartbeat, 10U, HAL_UART_RXEVENT_IDLE);
    CHECK(parse(&sm, buffer, uart_poll_rx(0, buffer, sizeof(buffer))) == 0);
    for (unsigned i = 0U; i < 9U; ++i) receive(heartbeat, 16U, HAL_UART_RXEVENT_TC);
#if defined(TEST_B1_LOG_CONSOLE)
    uint32_t before_gap = tick;
    uart_wait_rx(0, 1U);
    CHECK(tick == before_gap); /* 断流标记直接交给poll，wait不能清除或延迟它。 */
#endif
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == UART_RX_GAP);
    tlv_sm_reset(&sm); /* 对应RK任务的断流消费规则。 */
    CHECK(uart_get_rx_diagnostics(0, &diag) && diag.queue_overflows == 1U && diag.dropped_blocks == 9U);
    receive(heartbeat + 10U, 6U, HAL_UART_RXEVENT_IDLE);
    CHECK(parse(&sm, buffer, uart_poll_rx(0, buffer, sizeof(buffer))) == 0);
    receive(heartbeat, 16U, HAL_UART_RXEVENT_IDLE);
    CHECK(parse(&sm, buffer, uart_poll_rx(0, buffer, sizeof(buffer))) == 1);

    /* DMA满块也须完整交付，不能截断末尾或当作半传输事件重复交付。 */
    memset(buffer, 0xA7, sizeof(buffer));
    receive(buffer, sizeof(buffer), HAL_UART_RXEVENT_TC);
    memset(buffer, 0, sizeof(buffer));
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == UART_RX_BUF_SIZE);
    for (unsigned i = 0U; i < sizeof(buffer); ++i) CHECK(buffer[i] == 0xA7);
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == 0);

    fail_start[0] = 1; tick = 50U;
    receive(heartbeat, 16U, HAL_UART_RXEVENT_IDLE);
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == UART_RX_GAP && aborts == 0U);
    tick = 100U; CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == UART_RX_GAP && aborts == 1U);
    tick = 199U; CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == 0 && aborts == 1U);
    fail_start[0] = 0; fail_abort = 1; tick = 200U;
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == 0 && aborts == 2U);
    fail_abort = 0; tick = 300U;
    CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == 0 && aborts == 3U);
    CHECK(uart_get_rx_diagnostics(0, &diag) && diag.restart_failures == 2U &&
          diag.recovery_failures == 1U && diag.recoveries == 1U && !diag.recovery_pending);
    error_event(0x0CU); receive(heartbeat, 16U, HAL_UART_RXEVENT_IDLE);
    tick = 400U; CHECK(uart_poll_rx(0, buffer, sizeof(buffer)) == UART_RX_GAP);
    CHECK(uart_get_rx_diagnostics(0, &diag) && diag.errors == 1U && diag.last_hal_error == 0x0CU);
    receive(heartbeat, 16U, HAL_UART_RXEVENT_IDLE);
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

#if defined(TEST_B1_SERIAL_NET)
static void test_serial_upload(void)
{
    static const uint16_t payloads[] = {512U, 1024U, 1468U};
    static sn_frame_t request, decoded;
    static sn_parser_t parser;
    static uint8_t wire[SN_WIRE_MAX], block[UART_RX_BUF_SIZE];
    /* 以太网/IP/UDP/TFTP头共46字节；上传由RK一次发完整COBS帧，再等待MCU应答。 */
    const uint16_t network_headers = 14U + 20U + 8U + 4U;
    request.op = SN_SEND; request.session = 1U;
    for (unsigned i = 0U; i < sizeof(payloads) / sizeof(payloads[0]); ++i) {
        request.sequence = i + 1U;
        request.length = network_headers + payloads[i];
        memset(request.data, 0x5A, request.length);
        uint16_t length = sn_pack(&request, wire, sizeof(wire));
        /* 必须在帧尾IDLE前装下整帧，不能靠NORMAL DMA中途停收重启承接尾部。 */
        CHECK(length > request.length && length < UART_RX_BUF_SIZE);
        unsigned before = starts[0];
        receive(wire, length, HAL_UART_RXEVENT_IDLE);
        CHECK(starts[0] == before + 1U);
        CHECK(uart_poll_rx(0, block, sizeof(block)) == length);
        sn_parser_reset(&parser);
        for (unsigned j = 0U; j < length; ++j)
            CHECK(sn_feed(&parser, block[j], &decoded) == (j + 1U == length ? 1 : 0));
        CHECK(decoded.op == SN_SEND && decoded.sequence == request.sequence);
        CHECK(decoded.length == request.length && !memcmp(decoded.data, request.data, request.length));
        CHECK(uart_poll_rx(0, block, sizeof(block)) == 0);
    }
    CHECK(UART_RX_BUF_SIZE > SN_WIRE_MAX);
    message("PASS: MCU UART RX accepts 512/1024/1468 TFTP upload blocks before DMA restart\n");
}
#endif

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
        sample.voltage_mv[i] = 1000 + (int)i * 100; sample.idd[i].current_ua = (i + 1U) * 1000U;
    }
    CHECK(idd_stats_feed(&stats, 10U, 11U, &sample) == 1);
    CHECK(idd_stats_feed(&stats, 10U, 11U, &sample) == 0); /* 重复不累计。 */
    CHECK(idd_stats_feed(&stats, 8U, 9U, &sample) == 0 && stats.state == IDD_STATS_RUNNING);
    for (unsigned i = 0U; i < 3U; ++i) { sample.voltage_mv[i] += 200; sample.idd[i].current_ua += 2000U; }
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
    sample.voltage_mv[0] = INT32_MIN; sample.idd[0].current_ua = UINT32_MAX;
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

/* 快电流和慢电压分别馈入，不因另一组的缓存valid位重复统计或误计采样失败。 */
static void test_stats_groups(void)
{
    const uint8_t voltage = MEMORY_MONITOR_VALID_VOLTAGE;
    const uint8_t current = MEMORY_MONITOR_VALID_IDD;
    idd_stats_t stats = {0};
    memory_monitor_sample_t sample = {0};
    sample.valid_mask = voltage | current;
    sample.idd[0].current_ua = 100U;
    CHECK(idd_stats_begin(&stats, 100U, 10U, 1U) == IF_OK);
    CHECK(idd_stats_feed_groups(NULL, 100U, 100U, &sample, current) == IF_ERR);
    CHECK(idd_stats_feed_groups(&stats, 100U, 100U, NULL, current) == IF_ERR);
    CHECK(idd_stats_feed_groups(&stats, 100U, 100U, &sample, MEMORY_MONITOR_VALID_TPS_STATUS) == IF_ERR);
    CHECK(idd_stats_feed_groups(&stats, 100U, 100U, &sample, 0U) == 0 && stats.attempts == 0U);
    CHECK(idd_stats_feed_groups(&stats, 100U, 100U, &sample, current) == 1);
    CHECK(stats.voltage[0].valid_count == 0U && stats.voltage[0].failed_count == 0U);
    sample.idd[0].current_ua = 900U;
    CHECK(idd_stats_feed_groups(&stats, 101U, 101U, &sample, current) == 1);
    CHECK(stats.current[0].mean == 500 && stats.current[0].minimum == 100 && stats.current[0].maximum == 900);
    CHECK(idd_stats_feed_groups(&stats, 100U, 101U, &sample, voltage) == 1);
    CHECK(stats.voltage[0].valid_count == 1U && stats.current[0].valid_count == 2U);
    CHECK(idd_stats_feed_groups(&stats, 100U, 101U, &sample, voltage | current) == 0);
    CHECK(idd_stats_feed_groups(&stats, 100U, 100U, &sample, voltage | current) == 0);
    CHECK(idd_stats_feed_groups(&stats, 103U, 102U, &sample, voltage) == 0);
    CHECK(idd_stats_feed_groups(&stats, 99U, 102U, &sample, voltage) == 0);
    CHECK(stats.voltage[0].failed_count == 0U && stats.current[0].failed_count == 0U);
    sample.valid_mask = 0U;
    CHECK(idd_stats_feed_groups(&stats, 102U, 102U, &sample, current) == 1);
    CHECK(stats.current[0].failed_count == 1U && stats.voltage[0].failed_count == 0U);
    /* 同一时间电流已处理；双组包装只接纳尚未处理的电压组。 */
    sample.valid_mask = voltage | current;
    CHECK(idd_stats_feed_groups(&stats, 102U, 102U, &sample, voltage | current) == 1);
    CHECK(stats.current[0].valid_count == 2U && stats.current[0].failed_count == 1U);
    CHECK(stats.voltage[0].valid_count == 2U && stats.attempts == 5U);
    CHECK(idd_stats_feed_groups(&stats, 109U, 110U, &sample, current) == 0);
    CHECK(stats.state == IDD_STATS_COMPLETE);

    /* 只对越界轨记失败；其他轨仍有效，UINT64_MAX不得强转成负数或小电流。 */
    CHECK(idd_stats_begin(&stats, 0U, INT32_MAX, 1U) == IF_OK);
    sample.idd[0].current_ua = (uint64_t)UINT32_MAX + 1U;
    sample.idd[1].current_ua = UINT64_MAX;
    sample.idd[2].current_ua = UINT32_MAX;
    CHECK(idd_stats_feed_groups(&stats, 0U, 0U, &sample, current) == 1);
    CHECK(stats.current[0].valid_count == 0U && stats.current[0].failed_count == 1U);
    CHECK(stats.current[1].valid_count == 0U && stats.current[1].failed_count == 1U);
    CHECK(stats.current[2].valid_count == 1U && stats.current[2].sum == UINT32_MAX);
    /* 等价构造窗口末尾前的最大合法累加值，只执行最后一次，检查int64边界。 */
    stats.current[2].valid_count = INT32_MAX - 1U;
    stats.current[2].sum = (int64_t)UINT32_MAX * (INT32_MAX - 1U);
    stats.current_offset_ms = INT32_MAX - 2U;
    CHECK(idd_stats_feed_groups(&stats, INT32_MAX - 1U, INT32_MAX - 1U, &sample, current) == 1);
    CHECK(stats.current[2].valid_count == INT32_MAX);
    CHECK(stats.current[2].sum == (int64_t)UINT32_MAX * INT32_MAX);
    CHECK(stats.current[2].mean == UINT32_MAX && stats.current[2].sum > 0);
    idd_stats_cancel(&stats);
    CHECK(idd_stats_begin(&stats, UINT32_MAX - 1U, 10U, 1U) == IF_OK);
    sample.idd[0].current_ua = 100U;
    CHECK(idd_stats_feed_groups(&stats, UINT32_MAX, 0U, &sample, current) == 1);
    CHECK(idd_stats_feed_groups(&stats, UINT32_MAX, 0U, &sample, voltage) == 1);
    CHECK(stats.current[0].valid_count == 1U && stats.voltage[0].valid_count == 1U);
    stats.discarded = UINT32_MAX;
    CHECK(idd_stats_feed_groups(&stats, UINT32_MAX, 0U, &sample, current) == 0);
    CHECK(stats.discarded == UINT32_MAX);
}

#if defined(TEST_B1_LOG_CONSOLE)
/* 实际日志模块加实际三槽路由；只替代队列、DMA及TX出口，不另写选择器模型。 */
int _write(int file, char *data, int length);
static void clear_console_output(void)
{ console_output_length = 0U; console_output[0] = '\0'; }
static void console_input(const char *text)
{
    receive_port(UART_PORT_CONSOLE, (const uint8_t *)text, (uint16_t)strlen(text), HAL_UART_RXEVENT_IDLE);
    log_console_process();
}
static void console_bytes(const uint8_t *bytes, uint16_t length)
{
    receive_port(UART_PORT_CONSOLE, bytes, length, HAL_UART_RXEVENT_IDLE);
    log_console_process();
}
static void clear_rk_console_output(void) { rk_console_output_length = 0U; }
static void check_rk_console_output(const void *bytes, size_t length)
{
    CHECK(rk_console_output_length == length);
    CHECK(length == 0U || memcmp(rk_console_output, bytes, length) == 0);
}
static void rk_log_input(const char *text)
{
    receive_port(UART_PORT_RK_DEBUG, (const uint8_t *)text, (uint16_t)strlen(text), HAL_UART_RXEVENT_IDLE);
    log_console_process();
}
static void test_console_led(void)
{
    static const result_led_board_map_t map = {.pairs = {
        {&led_gpio, 1U, &led_gpio, 2U},
        {&led_gpio, 4U, &led_gpio, 8U},
        {&led_gpio, 16U, &led_gpio, 32U}
    }};
    result_led_state_t states[RESULT_LED_TARGET_COUNT];
    console_input("log mcu\n");
    led_gpio.unused = 0x40U; /* 模拟PG6已亮；所有验灯操作都不得改变该位。 */
    CHECK(result_led_init(&map) == HAL_OK);
    console_input("led 0 pass\n"); CHECK(led_gpio.unused == 0x42U);
    console_input("led 1 fail\r\n"); CHECK(led_gpio.unused == 0x44U);
    console_input("led 2 pass\n"); CHECK(led_gpio.unused == 0x60U);
    console_input("led all pass\n"); CHECK(led_gpio.unused == 0x6AU);
    console_input("led all fail\n"); CHECK(led_gpio.unused == 0x55U);
    console_input("led all off\n"); CHECK(led_gpio.unused == 0x40U);

    /* 三组逐一验证00/01/10/11与真实GPIO位对应；11跨两次半周期仍保持常亮。 */
    static const unsigned pin_levels[] = {0U, 2U, 1U, 3U};
    for (unsigned group = 0U; group < RESULT_LED_TARGET_COUNT; ++group) {
        for (unsigned bits = 0U; bits < 4U; ++bits) {
            char command[] = "led 0 00\n";
            command[4] = (char)('0' + group);
            command[6] = (bits & 2U) ? '1' : '0';
            command[7] = (bits & 1U) ? '1' : '0';
            console_input(command);
            unsigned expected = 0x40U | (pin_levels[bits] << (group * 2U));
            CHECK(led_gpio.unused == expected);
            tick += 500U; result_led_process(tick); CHECK(led_gpio.unused == expected);
            tick += 500U; result_led_process(tick); CHECK(led_gpio.unused == expected);
        }
    }
    console_input("led all 11\n"); CHECK(led_gpio.unused == 0x7FU);
    CHECK(result_led_get_all(states) == HAL_OK && states[0] == RESULT_LED_DEBUG_BOTH_ON);
    console_input("led all 00\n"); CHECK(led_gpio.unused == 0x40U);

    /* 半行不执行；常温/高温均使用正式500ms闪烁，而非重复串口指令模拟。 */
    console_input("led 0 nor"); CHECK(led_gpio.unused == 0x40U);
    console_input("mal\n"); CHECK(led_gpio.unused == 0x43U);
    tick += 499U; result_led_process(tick); CHECK(led_gpio.unused == 0x43U);
    tick += 1U; result_led_process(tick); CHECK(led_gpio.unused == 0x40U);
    tick += 500U; result_led_process(tick); CHECK(led_gpio.unused == 0x43U);
    console_input("led all hot\n"); CHECK(led_gpio.unused == 0x55U);
    tick += 500U; result_led_process(tick); CHECK(led_gpio.unused == 0x40U);
    CHECK(result_led_get_all(states) == HAL_OK);
    for (unsigned i = 0U; i < RESULT_LED_TARGET_COUNT; ++i)
        CHECK(states[i] == RESULT_LED_TEST_HOT);

    /* 非法编号、模式、尾随内容及断流不能覆盖原状态。 */
    console_input("led 3 pass\nled -1 pass\nled all nope\nled 0 pass x\n");
    console_input("led 0 0\nled 0 12\nled 0 011\nled 0 11 x\n");
    CHECK(result_led_get_all(states) == HAL_OK && states[0] == RESULT_LED_TEST_HOT);
    console_input("led all passXXXXXXXXXXXXXXXX\n");
    CHECK(result_led_get_all(states) == HAL_OK && states[0] == RESULT_LED_TEST_HOT);
    console_input("led all ");
    uart[2].ErrorCode = 8U;
    isr = 1; HAL_UART_ErrorCallback(&uart[2]); isr = 0;
    tick += 100U; log_console_process(); console_input("pass\n");
    CHECK(result_led_get_all(states) == HAL_OK && states[0] == RESULT_LED_TEST_HOT);

    console_input("led all off\nlog rk\n");
    clear_rk_console_output();
    console_input("led all pass\n");
    check_rk_console_output("led all pass\n", 13U);
    clear_rk_console_output();
    console_input("led 0 11\n");
    check_rk_console_output("led 0 11\n", 9U);
    CHECK(led_gpio.unused == 0x40U); /* RK模式只转发，不能暗中设置本地灯。 */
    clear_rk_console_output();
    console_input("log mcu\n");
    clear_console_output();
    message("PASS: B1 console LED modes, GPIO isolation and 500ms blink\n");
}

static void test_console_power(void)
{
    console_input("log mcu\n");
    CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_NONE);
    console_input("start rk x\nstart rK\nend rk x\n");
    CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_NONE);
    console_input("sta"); CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_NONE);
    console_input("rt rk\r\n");
    CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_START_RK);
    CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_NONE);
    clear_console_output(); console_input("end rk\nstart rk\n");
    CHECK(strstr(console_output, "busy") && log_console_take_power_request() == LOG_CONSOLE_POWER_NONE);
    log_console_complete_power_request(NULL); clear_console_output(); log_console_process();
    CHECK(strstr(console_output, "1800/1100/600mV; fspiM1") && strstr(console_output, "power ON"));
    console_input("end rk\n");
    CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_END_RK);
    clear_console_output(); log_console_complete_power_request(NULL); log_console_process();
    CHECK(strstr(console_output, "power OFF"));

    /* 入队通知未发出前不领取；完成回复遇拥塞保留，重复命令不覆盖它。 */
    fail_console_send = 1; console_input("start rk\n");
    CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_NONE);
    fail_console_send = 0; log_console_process();
    CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_START_RK);
    fail_console_send = 1; log_console_complete_power_request("executionFailed");
    log_console_process(); console_input("end rk\n");
    CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_NONE);
    clear_console_output(); fail_console_send = 0;
    log_console_process(); log_console_process();
    CHECK(strstr(console_output, "start rk failed: executionFailed"));
    console_input("end rk\n");
    CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_END_RK);
    console_input("log rk\n");
    clear_console_output(); log_console_complete_power_request(NULL); log_console_process();
    CHECK(strstr(console_output, "power OFF")); /* 切换日志来源不丢已受理动作的结论。 */
    clear_rk_console_output(); console_input("start rk\nend rk\n");
    CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_NONE);
    check_rk_console_output("start rk\nend rk\n", 16U);
    console_input("log mcu\nstart ");
    uart[2].ErrorCode = 8U;
    isr = 1; HAL_UART_ErrorCallback(&uart[2]); isr = 0;
    tick += 100U; log_console_process(); console_input("rk\n");
    CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_NONE);
    clear_console_output(); clear_rk_console_output();
    message("PASS: B1 console power command framing, single slot, backpressure and mode isolation\n");
}

static void test_log_console(void)
{
    uint8_t buffer[UART_RX_BUF_SIZE];
    unsigned saved_protocol_sends = protocol_send_count;
    CHECK(log_console_write("early", 5U) == 0U);
    fail_start[1] = 0; tick += 100U;
    (void)uart_poll_rx(UART_PORT_RK_DEBUG, buffer, sizeof(buffer));
    log_console_start();
    CHECK((console_output_length != 0U) == (B1_LOG_DEFAULT_SOURCE == 0));
    clear_console_output();
    CHECK(log_console_write("mcu", 3U) == 3U);
    rk_log_input("rk");
    CHECK(strcmp(console_output, B1_LOG_DEFAULT_SOURCE == 0 ? "mcu" : "rk") == 0);

    console_input("log mcu\r\n");
    clear_console_output();
    /* 半行不能切换；逐字节与CRLF分片都合法。 */
    console_input("log r");
    CHECK(log_console_write("still-mcu", 9U) == 9U);
    CHECK(strcmp(console_output, "still-mcu") == 0);
    clear_console_output();
    console_input("k\r"); console_input("\n");
    CHECK(strcmp(console_output, "\r\n[log] source=rk\r\n") == 0);
    check_rk_console_output(NULL, 0U); /* 本地切换CRLF不能把LF漏给RK。 */
    clear_console_output();
    CHECK(log_console_write("hidden", 6U) == 6U && console_output_length == 0U);
    /* RK输出即使包含命令文字，也必须原样转发而不能解释。 */
    rk_log_input("log mcu\nRK text\r\n");
    CHECK(strcmp(console_output, "log mcu\nRK text\r\n") == 0);
    CHECK(log_console_write("hidden", 6U) == 6U);
    CHECK(strcmp(console_output, "log mcu\nRK text\r\n") == 0);

    clear_console_output();
    console_input("log unknown\n");
    CHECK(console_output_length == 0U);
    check_rk_console_output("log unknown\n", 12U);
    clear_rk_console_output();
    console_input("status\r\n");
    check_rk_console_output("status\r\n", 8U);
    CHECK(log_console_take_status_request() == 0);
    clear_rk_console_output();
    clear_console_output();
    static const uint8_t bad_nul[] = {'l','o','g',' ','m','c','u',0,'\n'};
    console_bytes(bad_nul, sizeof(bad_nul));
    CHECK(strstr(console_output, "source=mcu") == NULL);
    check_rk_console_output(bad_nul, sizeof(bad_nul));
    clear_rk_console_output();
    memset(buffer, 'x', sizeof(buffer) - 8U);
    memcpy(buffer + sizeof(buffer) - 8U, "log mcu\n", 8U);
    console_bytes(buffer, sizeof(buffer));
    check_rk_console_output(buffer, sizeof(buffer)); /* 长行及行中退出字样原样发送。 */
    CHECK(strstr(console_output, "source=mcu") == NULL);
    clear_rk_console_output();
    /* 最坏块边界：上一块保留7字节候选，下一块满长且首字节失配。 */
    console_input("log mcu");
    check_rk_console_output(NULL, 0U);
    memset(buffer, 'x', sizeof(buffer)); buffer[sizeof(buffer) - 1U] = '\n';
    console_bytes(buffer, sizeof(buffer));
    CHECK(rk_console_output_length == sizeof(buffer) + 7U);
    CHECK(memcmp(rk_console_output, "log mcu", 7U) == 0);
    CHECK(memcmp(rk_console_output + 7U, buffer, sizeof(buffer)) == 0);
    clear_rk_console_output();
    clear_console_output();
    rk_log_input("still-rk");
    CHECK(strcmp(console_output, "still-rk") == 0);

    /* 任意控制字节原样发送，退出命令仅在新行首识别。 */
    static const uint8_t controls[] = {'x',0x08,0x7F,0x03,0x09,0x1B,'[','A',0,0x80,0xFF,'\r','\n'};
    console_bytes(controls, sizeof(controls));
    check_rk_console_output(controls, sizeof(controls));
    clear_rk_console_output();
    console_input("log m");
    check_rk_console_output(NULL, 0U);
    tick += 60000U; log_console_process();
    check_rk_console_output(NULL, 0U); /* 输入停顿不把候选前缀送给RK。 */
    console_input("cu\r"); console_input("\n");
    CHECK(strstr(console_output, "source=mcu") != NULL);
    check_rk_console_output(NULL, 0U);
    console_input("log rk\r\n"); clear_console_output();
    /* 暂存的退出候选支持退格；Ctrl+C放弃候选并立即发往RK。 */
    console_input("log mc\bcu\n");
    CHECK(strstr(console_output, "source=mcu") != NULL);
    check_rk_console_output(NULL, 0U);
    console_input("log rk\n"); clear_console_output();
    console_input("log mc\x7F" "cu\n");
    CHECK(strstr(console_output, "source=mcu") != NULL);
    console_input("log rk\n"); clear_console_output();
    console_input("log m");
    static const uint8_t interrupt[] = {0x03};
    console_bytes(interrupt, sizeof(interrupt));
    check_rk_console_output(interrupt, sizeof(interrupt));
    clear_rk_console_output();
    console_input("log mcu\n");
    CHECK(strstr(console_output, "source=mcu") != NULL);
    check_rk_console_output(NULL, 0U);
    console_input("log rk\n"); clear_console_output();

    /* RK TX背压保留当前块；停止消费后续输入，但RK日志仍持续处理。 */
    fail_rk_console_send = 1;
    console_input("printenv\r\n");
    check_rk_console_output(NULL, 0U);
    receive_port(UART_PORT_CONSOLE, (const uint8_t *)"version\n", 8U, HAL_UART_RXEVENT_IDLE);
    unsigned pending_input_blocks = queues[2].count;
    CHECK(pending_input_blocks == 1U);
    rk_log_input("rk-during-backpressure");
    CHECK(queues[2].count == pending_input_blocks);
    CHECK(strstr(console_output, "rk-during-backpressure") != NULL);
    check_rk_console_output(NULL, 0U);
    fail_rk_console_send = 0;
    for (unsigned i = 0U; i < 4U; ++i) log_console_process();
    check_rk_console_output("printenv\r\nversion\n", 18U);
    log_console_process();
    check_rk_console_output("printenv\r\nversion\n", 18U); /* 恢复不重复排队。 */
    clear_rk_console_output();

    /* 控制台断流不能把后续命令尾部拼成有效切换；协议队列始终不被消费。 */
    console_input("log m");
    uart[2].ErrorCode = 8U;
    isr = 1; HAL_UART_ErrorCallback(&uart[2]); isr = 0;
    clear_console_output();
    tick += 100U; log_console_process();
    CHECK(console_output_length != 0U); /* 输入断流必须通知调试台。 */
    console_input("cu\n");
    CHECK(strstr(console_output, "source=mcu") == NULL);
    check_rk_console_output(NULL, 0U);
    uint8_t heartbeat[16];
    CHECK(tlv_pack_heartbeat(77U, heartbeat, sizeof(heartbeat)) == 16);
    receive(heartbeat, sizeof(heartbeat), HAL_UART_RXEVENT_IDLE);
    for (unsigned i = 0U; i < 3U; ++i)
        receive_port(UART_PORT_RK_DEBUG, (const uint8_t *)"rk", 2U, HAL_UART_RXEVENT_IDLE);
    log_console_process();
    CHECK(uart_poll_rx(UART_PORT_RK_PROTOCOL, buffer, sizeof(buffer)) == 16);
    CHECK(memcmp(buffer, heartbeat, 16U) == 0);
    clear_console_output(); rk_log_input("after-log-overflow");
    CHECK(strcmp(console_output, "after-log-overflow") == 0);

    /* TX拥塞时先保留控制回复；恢复出口后先确认再输出新来源，不能死等。 */
    clear_console_output(); fail_console_send = 1;
    console_input("log mcu\n");
    CHECK(console_output_length == 0U);
    CHECK(log_console_write("drop-until-ack", 14U) == 14U);
    fail_console_send = 0; log_console_process();
    CHECK(strcmp(console_output, "\r\n[log] source=mcu\r\n") == 0);
    clear_console_output();
    rk_log_input("discard-rk-now");
    CHECK(console_output_length == 0U);
    char big[300]; memset(big, 'm', sizeof(big));
    CHECK(log_console_write(big, sizeof(big)) == sizeof(big));
    CHECK(console_output_length == sizeof(big) && memcmp(console_output, big, sizeof(big)) == 0);
    /* 最大协议记录必须一次入USART1；出口满则整条丢弃，恢复后不补发。 */
    clear_console_output();
    unsigned before = console_send_count;
    CHECK(log_console_mcu_selected());
    log_console_write_record(big, LOG_CONSOLE_RECORD_SIZE);
    CHECK(console_send_count == before + 1U && console_output_length == LOG_CONSOLE_RECORD_SIZE);
    CHECK(memcmp(console_output, big, LOG_CONSOLE_RECORD_SIZE) == 0);
    fail_console_send = 1;
    uint32_t lost = log_console_dropped_bytes();
    log_console_write_record(big, LOG_CONSOLE_RECORD_SIZE);
    CHECK(log_console_dropped_bytes() == lost + LOG_CONSOLE_RECORD_SIZE);
    CHECK(console_output_length == LOG_CONSOLE_RECORD_SIZE);
    CHECK(log_console_write("full", 4U) == 0U);
    fail_console_send = 0;

    clear_console_output();
    CHECK(_write(1, "stdio", 5) == 5 && strcmp(console_output, "stdio") == 0);
    CHECK(_write(0, "no", 2) == -1 && _write(1, NULL, 1) == -1);
    CHECK(_write(2, NULL, 0) == 0 && _write(1, "no", -1) == -1);
    isr = 1; CHECK(log_console_write("no-isr", 6U) == 0U); isr = 0;
    /* 状态查询只触发本地只读请求，分片、拥塞和来源切换不能误执行。 */
    CHECK(log_console_take_status_request() == 0);
    console_input("sta");
    CHECK(log_console_take_status_request() == 0);
    console_input("tus\r\n");
    CHECK(log_console_take_status_request() == 1);
    CHECK(log_console_take_status_request() == 0);
    console_input("status\nstatus\n");
    CHECK(log_console_take_status_request() == 1);
    CHECK(log_console_take_status_request() == 0);
    fail_console_send = 1;
    console_input("status\n");
    CHECK(log_console_take_status_request() == 0);
    CHECK(log_console_write_status("report", 6U) == 0);
    fail_console_send = 0;
    log_console_process();
    CHECK(log_console_take_status_request() == 1);
    clear_console_output();
    CHECK(log_console_write_status("report", 6U) == 1);
    CHECK(strcmp(console_output, "report") == 0);
    fail_console_send = 1;
    CHECK(log_console_write_status("retry", 5U) == 0);
    fail_console_send = 0;
    CHECK(log_console_write_status(NULL, 1U) == -1);
    CHECK(log_console_write_status(big, sizeof(big)) == -1);
    isr = 1; CHECK(log_console_write_status("isr", 3U) == -1); isr = 0;
    console_input("status\nlog rk\n");
    CHECK(log_console_take_status_request() == 0);
    CHECK(log_console_write_status("cancel", 6U) == -1);
    clear_console_output();
    console_input("status\n");
    CHECK(log_console_take_status_request() == 0);
    CHECK(console_output_length == 0U);
    check_rk_console_output("status\n", 7U);
    clear_rk_console_output();
    CHECK(_write(2, "hidden", 6) == 6 && console_output_length == 0U);
    CHECK(!log_console_mcu_selected());
    lost = log_console_dropped_bytes();
    log_console_write_record(big, LOG_CONSOLE_RECORD_SIZE);
    CHECK(console_output_length == 0U && log_console_dropped_bytes() == lost);
    check_rk_console_output(NULL, 0U);
    test_console_led();
    test_console_power();
    /* 默认值在重新初始化后恢复，两种编译配置各执行一次本用例。 */
    log_console_start(); clear_console_output();
    CHECK(log_console_write("mcu", 3U) == 3U); rk_log_input("rk");
    CHECK(strcmp(console_output, B1_LOG_DEFAULT_SOURCE == 0 ? "mcu" : "rk") == 0);
    CHECK(protocol_send_count == saved_protocol_sends); /* 调试双向转发绝不能写UART4。 */
    CHECK(!critical && !isr);
    message("PASS: B1 selectable log console\n");
}
#endif

#if defined(TEST_NET_CONSOLE)
extern uint32_t fake_net_session;
extern int fake_net_blocked;
extern char fake_net_output[];
extern size_t fake_net_output_length, fake_net_input_length;
extern uint8_t fake_net_input[];

static void network_input(const char *text)
{
    CHECK(fake_net_input_length == 0U && strlen(text) <= 512U);
    fake_net_input_length = strlen(text);
    memcpy(fake_net_input, text, fake_net_input_length);
    log_console_process();
}
static void clear_network_output(void)
{
    fake_net_output_length = 0U;
    fake_net_output[0] = '\0';
}

static void test_network_console(void)
{
    console_input("log mcu\n");
    fake_net_session = 1U;
    log_console_process();
    CHECK(strstr(fake_net_output, "connected; source=mcu"));
    clear_network_output(); clear_console_output();
    log_console_write_record("both\r\n", 6U);
    CHECK(!strcmp(fake_net_output, "both\r\n") && !strcmp(console_output, "both\r\n"));

    /* 两端半行不互拼；status只返回发起端，切换另一端不会取消报告。 */
    network_input("sta"); console_input("log rk\n"); network_input("tus\n");
    CHECK(log_console_take_status_request());
    clear_network_output(); clear_console_output();
    CHECK(log_console_write_status("snapshot", 8U) == 1);
    CHECK(!strcmp(fake_net_output, "snapshot") && !console_output_length);
    CHECK(log_console_mcu_selected());
    clear_network_output(); log_console_write_record("mcu\r\n", 5U);
    CHECK(!strcmp(fake_net_output, "mcu\r\n") && !console_output_length);
    rk_log_input("rk text"); CHECK(!strcmp(console_output, "rk text"));

    /* 两端可同时观察RK，但只有先进入者的按键会写入USART2。 */
    network_input("log rk\n"); CHECK(strstr(fake_net_output, "read-only"));
    clear_rk_console_output(); network_input("version\n"); check_rk_console_output(NULL, 0U);
    console_input("version\n"); check_rk_console_output("version\n", 8U);
    clear_rk_console_output(); console_input("log mcu\n");
    network_input("log mcu\nlog rk\n");
    network_input("\x1b[A\r\n"); check_rk_console_output("\x1b[A\r\n", 5U);
    console_input("log rk\n"); CHECK(strstr(console_output, "read-only"));
    clear_rk_console_output(); console_input("version\n"); check_rk_console_output(NULL, 0U);

    /* 断线丢弃输入半行并释放RK控制权，不向新连接送旧状态/电源回复。 */
    network_input("log mcu\nstatus\n"); CHECK(log_console_take_status_request());
    fake_net_session = 0U; log_console_process();
    CHECK(log_console_write_status("old", 3U) == -1);
    fake_net_session = 2U; log_console_process();
    network_input("start "); fake_net_session = 3U; log_console_process(); network_input("rk\n");
    CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_NONE);
    network_input("start rk\n"); CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_START_RK);
    fake_net_session = 4U; log_console_process(); clear_network_output();
    log_console_complete_power_request(NULL); log_console_process();
    CHECK(!strstr(fake_net_output, "power ON"));
    network_input("end rk\n"); CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_END_RK);
    log_console_complete_power_request(NULL); log_console_process();
    CHECK(strstr(fake_net_output, "power OFF"));

    /* 网络拥塞不阻塞串口日志和命令；两路共用电源单槽，不能并发动作。 */
    fake_net_blocked = 1; network_input("status\n");
    console_input("log mcu\n"); clear_console_output();
    log_console_write_record("serial alive\r\n", 14U);
    CHECK(!strcmp(console_output, "serial alive\r\n"));
    console_input("start rk\n"); CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_START_RK);
    fake_net_blocked = 0; log_console_process();
    network_input("end rk\n"); CHECK(strstr(fake_net_output, "busy"));
    CHECK(log_console_take_power_request() == LOG_CONSOLE_POWER_NONE);
    log_console_complete_power_request(NULL); log_console_process();
    network_input("log rk\n"); fake_net_session = 0U; log_console_process();
    console_input("log rk\n"); clear_rk_console_output();
    console_input("version\n"); check_rk_console_output("version\n", 8U);
    console_input("log mcu\n");
    CHECK(!critical && !isr);
    message("PASS: network/serial source isolation, RK input ownership, status routing and reconnect\n");
}
#endif

void test_main(void)
{
    test_frames(); test_uart(); test_stats(); test_stats_groups();
#if defined(TEST_B1_SERIAL_NET)
    test_serial_upload();
#endif
#if defined(TEST_B1_LOG_CONSOLE)
    test_log_console();
#endif
#if defined(TEST_NET_CONSOLE)
    test_network_console();
#endif
    message("PASS: UART recovery, RK bounds and IDD windows\n");
    finish(0U);
}
