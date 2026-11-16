/* 实际MCU模块离线执行：用RAM代替外设/队列，显式推进tcpip回调，不连接硬件。 */
#include "main.h"
static ETH_TypeDef test_eth;
#undef ETH
#define ETH (&test_eth)
#include "handoff_mcu.c"
#include "protocol_trace.h"

#define TEXT0(x) #x
#define TEXT(x) TEXT0(x)
#define CHECK(x) do { if (!(x)) fail("FAIL " TEXT(__LINE__) " " #x "\n"); } while (0)
static void message(const char *s)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = s;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}
static _Noreturn void finish(uint32_t code)
{
    uint32_t block[] = {0x20026U, code};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) {}
}
static _Noreturn void fail(const char *s) { message(s); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }

static pwr_ctrl_state_t power_state = PWR_CTRL_STATE_OFF;
static int test_busy, power_busy, rk_idle = 1, uart_idle = 1, eth_idle = 1;
static int callback_reject, callback_count, arp_count, rx_count, tx_count, sem_ready;
static tcpip_callback_fn pending_callback;
static void *pending_argument;
static uint8_t packet_bytes[SN_ETH_MAX];
static struct pbuf packet_buffer = {.payload = packet_bytes};
static uint32_t now;
static unsigned rx_waits, idle_waits;
static char last_log[256];
static int log_result = 1, log_calls;
static uint8_t uart_input[SN_WIRE_MAX], last_tlv[TLV_FRAME_MAX];
static uint16_t uart_input_length, last_tlv_length;
static int uart_fail, jump_attempts, tlv_sends;
static uint32_t actual_baud = SN_BOOT_BAUD, baud_changes;
static int baud_fail;
static uint64_t heartbeat_sequence, jump_sequence;
QueueHandle_t rkRequestQueue = (QueueHandle_t)(uintptr_t)3U;
QueueHandle_t rkResponseQueue = (QueueHandle_t)(uintptr_t)4U;

void vPortEnterCritical(void) {}
void vPortExitCritical(void) {}
uint32_t HAL_GetTick(void) { return now; }
TickType_t xTaskGetTickCount(void) { return pdMS_TO_TICKS(now); }
void vTaskDelay(TickType_t ticks)
{
    CHECK(ticks == pdMS_TO_TICKS(HANDOFF_IDLE_WAIT_MS));
    ++idle_waits;
}
void uart_wait_rx(int id, uint32_t timeout_ms)
{
    CHECK(id == UART_PORT_RK_PROTOCOL && timeout_ms == HANDOFF_IDLE_WAIT_MS);
    ++rx_waits;
}
pwr_ctrl_state_t pwr_ctrl_get_state(void) { return power_state; }
int pc_test_service_busy(void) { return test_busy; }
int pc_power_service_busy(void) { return power_busy; }
UBaseType_t uxQueueMessagesWaiting(const QueueHandle_t queue)
{ CHECK(queue == rkRequestQueue || queue == rkResponseQueue); return rk_idle ? 0U : 1U; }
int uart_tx_idle(int id) { CHECK(id == UART_PORT_RK_PROTOCOL); return uart_idle; }
int uart_set_baud(int id, uint32_t baud)
{
    CHECK(id == UART_PORT_RK_PROTOCOL && uart_idle);
    if (baud_fail) { actual_baud = baud; return IF_ERR; }
    actual_baud = baud; ++baud_changes;
    return IF_OK;
}
int handoff_eth_tx_idle(void) { return eth_idle; }
size_t log_console_write(const char *data, size_t length)
{
    CHECK(length < sizeof(last_log));
    memcpy(last_log, data, length);
    last_log[length] = '\0';
    return length;
}
int log_console_write_status(const char *data, size_t length)
{
    CHECK(length > 0U && length <= 128U);
    ++log_calls;
    if (log_result == 1) (void)log_console_write(data, length);
    return log_result;
}
void task_watchdog_progress(uint32_t task_id) { CHECK(task_id == TASK_WATCHDOG_RK); }
int uart_poll_rx(int id, uint8_t *buffer, uint16_t capacity)
{
    CHECK(id == UART_PORT_RK_PROTOCOL);
    uint16_t length = uart_input_length < capacity ? uart_input_length : capacity;
    memcpy(buffer, uart_input, length);
    uart_input_length -= length;
    memmove(uart_input, uart_input + length, uart_input_length);
    return length;
}
int uart_send(int id, uint8_t *buffer, uint16_t length)
{
    CHECK(id == UART_PORT_RK_PROTOCOL);
    if (buffer[0] == TLV_SYNC_HEADER) {
        tlv_frame_t frame;
        CHECK(tlv_unpack(buffer, length, &frame) >= 0);
        memcpy(last_tlv, buffer, length);
        last_tlv_length = length;
        ++tlv_sends;
        if (frame.count == 0U) heartbeat_sequence = frame.sequence;
        if (frame.count == 1U && frame.units[0].type == TLV_TYPE_CMD &&
            frame.payload_used == 2U && frame.payload[0] == HANDOFF_RK_CMD_CONTINUE_BOOT &&
            frame.payload[1] == 0U) {
            ++jump_attempts;
            jump_sequence = frame.sequence;
        }
    }
    return uart_fail ? IF_ERR : IF_OK;
}
void rk_trace_ready(uint64_t sequence, int queried) { (void)sequence; (void)queried; }
void rk_trace_sent(uint16_t command, const uint8_t *wire, int length, int queued)
{ (void)command; (void)wire; (void)length; (void)queued; }
void rk_trace_received(const tlv_frame_t *frame) { (void)frame; }
void rk_trace_query_while_busy(const tlv_frame_t *frame) { (void)frame; }
void rk_trace_rejected(rk_rx_reject_reason_t reason, const tlv_frame_t *frame)
{ (void)reason; (void)frame; }
void rk_trace_progress(const tlv_frame_t *frame, int previous_valid, unsigned percent)
{ (void)frame; (void)previous_valid; (void)percent; }
void trace_count(trace_group_t group, trace_direction_t direction) { (void)group; (void)direction; }
BaseType_t xQueueGenericSend(QueueHandle_t queue, const void * const item,
                            TickType_t wait, const BaseType_t position)
{
    (void)wait; (void)position;
    if (queue == s_tx_done) { sem_ready = 1; return pdTRUE; }
    CHECK(queue == s_rx_queue && item);
    ++rx_count;
    return pdTRUE;
}
BaseType_t xQueueGenericReset(QueueHandle_t queue, BaseType_t new_queue)
{ (void)new_queue; CHECK(queue == s_rx_queue); rx_count = 0; return pdPASS; }
BaseType_t xQueueReceive(QueueHandle_t queue, void * const out, TickType_t wait)
{ (void)queue; (void)out; (void)wait; return pdFALSE; }
BaseType_t xQueueSemaphoreTake(QueueHandle_t queue, TickType_t wait)
{
    CHECK(queue == s_tx_done);
    now += wait;
    if (!sem_ready) return pdFALSE;
    sem_ready = 0;
    return pdTRUE;
}
err_t tcpip_try_callback(tcpip_callback_fn callback, void *argument)
{
    if (callback_reject) return ERR_MEM;
    CHECK(!pending_callback);
    pending_callback = callback;
    pending_argument = argument;
    ++callback_count;
    return ERR_OK;
}
static void run_callback(void)
{
    CHECK(pending_callback);
    tcpip_callback_fn callback = pending_callback;
    pending_callback = NULL;
    callback(pending_argument);
}
void etharp_cleanup_netif(struct netif *netif) { CHECK(netif == s_netif); }
err_t etharp_request(struct netif *netif, const ip4_addr_t *ip)
{ CHECK(netif == s_netif && ip->addr == s_ip.addr); ++arp_count; return ERR_OK; }
void netif_set_addr(struct netif *netif, const ip4_addr_t *ip,
                    const ip4_addr_t *mask, const ip4_addr_t *gateway)
{ netif->ip_addr = *ip; netif->netmask = *mask; netif->gw = *gateway; }
struct pbuf *pbuf_alloc(pbuf_layer layer, u16_t length, pbuf_type type)
{
    (void)layer; (void)type;
    CHECK(length <= sizeof(packet_bytes));
    packet_buffer.len = packet_buffer.tot_len = length;
    return &packet_buffer;
}
err_t pbuf_take(struct pbuf *p, const void *data, u16_t length)
{ memcpy(p->payload, data, length); return ERR_OK; }
u8_t pbuf_free(struct pbuf *p) { CHECK(p == &packet_buffer); return 1U; }
u16_t pbuf_copy_partial(const struct pbuf *p, void *data, u16_t length, u16_t offset)
{
    if (offset + length > p->tot_len) return 0U;
    memcpy(data, (const uint8_t *)p->payload + offset, length);
    return length;
}
static err_t link_output(struct netif *netif, struct pbuf *p)
{ CHECK(netif == s_netif && p == &packet_buffer); ++tx_count; return ERR_OK; }

static uint8_t transact(uint8_t op, uint32_t session)
{
    static sn_frame_t q, r;
    q.op = op; q.session = session; q.status = 0U; q.length = 0U;
    q.sequence = s_server.valid ? s_server.request.sequence + 1U : 1U;
    (void)sn_server_handle(&s_server, &q, &r, execute, NULL);
    s_response = r;
    return r.status;
}

static void arm_boot_test(void)
{
    CHECK(!handoff_mcu_busy() && power_state == PWR_CTRL_STATE_OFF);
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_RK));
    handoff_mcu_tcp_poll(0, 1);
    CHECK(handoff_mcu_rk_poll() == 1 && s_boot_phase == HANDOFF_BOOT_PROBE);
}

static void end_boot_test(void)
{
    power_state = PWR_CTRL_STATE_OFF;
    handoff_mcu_tcp_poll(0, 1);
    handoff_mcu_tcp_poll(0, 0); run_callback();
    handoff_mcu_tcp_poll(0, 0); handoff_mcu_tcp_poll(0, 1);
    CHECK(handoff_mcu_rk_poll() == 0 && !handoff_mcu_busy());
}

static void receive_heartbeat(uint64_t sequence)
{
    CHECK(!uart_input_length);
    uart_input_length = (uint16_t)tlv_pack_heartbeat(sequence, uart_input, sizeof(uart_input));
    CHECK(handoff_mcu_rk_poll() == 1);
}

static void receive_serial(sn_frame_t *request)
{
    CHECK(!uart_input_length);
    uart_input_length = sn_pack(request, uart_input, sizeof(uart_input));
    CHECK(uart_input_length && handoff_mcu_rk_poll() == 1);
}

/* 已授权会话中的真实串口解析：改速不能早于低速ACK的TC，丢ACK可低速重试。 */
static void test_baud(void)
{
    s_boot_phase = HANDOFF_BOOT_EDK;
    sn_frame_t request = {.op = SN_HELLO, .session = s_state.session,
        .sequence = s_server.request.sequence + 1U, .length = SN_RATE_REQUEST_SIZE};
    for (unsigned i = 0U; i < SN_RATE_REQUEST_SIZE; ++i)
        request.data[i] = (uint8_t)(SN_DATA_BAUD >> (i * 8U));
    s_state.authorized = 0U;
    receive_serial(&request);
    CHECK(s_response.status == SN_INVALID && !s_rate_pending && baud_changes == 0U);
    s_state.authorized = 1U; ++request.sequence;
    request.data[0] ^= 1U;
    receive_serial(&request);
    CHECK(s_response.status == SN_INVALID && !s_rate_pending);
    request.data[0] ^= 1U; ++request.sequence;
    uart_fail = 1;
    receive_serial(&request);
    CHECK(s_response.status == SN_OK && !s_rate_pending && actual_baud == SN_BOOT_BAUD);
    uart_fail = 0;
    receive_serial(&request); /* 回复入队失败后的同序号重试。 */
    CHECK(s_rate_pending && actual_baud == SN_BOOT_BAUD);
    uart_idle = 0;
    handoff_mcu_rk_poll();
    CHECK(s_rate_pending && actual_baud == SN_BOOT_BAUD && baud_changes == 0U);
    uart_idle = 1;
    handoff_mcu_rk_poll();
    CHECK(s_rate_trial && actual_baud == SN_DATA_BAUD && baud_changes == 1U);
    now += SN_RATE_TRIAL_MS;
    handoff_mcu_rk_poll();
    CHECK(!s_rate_trial && actual_baud == SN_BOOT_BAUD && baud_changes == 2U);
    receive_serial(&request);
    CHECK(s_rate_pending);
    handoff_mcu_rk_poll();
    CHECK(s_rate_trial && actual_baud == SN_DATA_BAUD);
    ++request.sequence; request.length = 0U;
    receive_serial(&request);
    CHECK(!s_rate_trial && s_response.status == SN_OK && s_response.length == SN_HELLO_INFO_SIZE);
    now += SN_RATE_TRIAL_MS * 2U;
    handoff_mcu_rk_poll();
    CHECK(actual_baud == SN_DATA_BAUD && baud_changes == 3U);
    receive_serial(&request); /* 高速确认ACK丢失，同序号再次答复。 */
    CHECK(s_response.status == SN_OK && actual_baud == SN_DATA_BAUD);
    message("PASS: MCU baud authorization, ACK enqueue/TC, fallback, duplicate and fast confirmation\n");
}

static void test_spl_boot(void)
{
    /* RK提供的固定样本只作独立金样；实际发送仍须使用本端流水号。 */
    static const uint8_t golden[] = {0x5A,0x00,0x06,0xD1,0xD1,0,0,0,0,0,0,0,2,
                                     0x50,0,0,2,0x3E,0,0x35,0x31,0xA5};
    uint8_t expected[TLV_FREQUENCY_FRAME_SIZE];
    CHECK(tlv_pack_cmd(HANDOFF_RK_CMD_CONTINUE_BOOT, NULL, 0U, 2U,
                      expected, sizeof(expected)) == sizeof(golden));
    CHECK(memcmp(expected, golden, sizeof(golden)) == 0);
    for (unsigned i = 0U; i < 5U; ++i) (void)rk_link_next_sequence();
    arm_boot_test();

    /* 启动过渡继续响应真实0x35，尚未ON不能发跳转或普通心跳。 */
    rk_link_frequency_begin(91U, 2133);
    power_state = PWR_CTRL_STATE_WAIT_SECOND_RAIL;
    uart_input_length = (uint16_t)tlv_pack_cmd(RK_CMD_QUERY_FREQUENCY, NULL, 0U, 0U,
                                              uart_input, sizeof(uart_input));
    handoff_mcu_rk_poll();
    tlv_frame_t reply;
    CHECK(tlv_unpack(last_tlv, last_tlv_length, &reply) == 2);
    CHECK(tlv_pack_frequency(2133, reply.sequence, expected, sizeof(expected)) == last_tlv_length);
    CHECK(memcmp(last_tlv, expected, last_tlv_length) == 0 && !jump_attempts);
    rk_link_frequency_end(91U);
    power_state = PWR_CTRL_STATE_ON;
    handoff_mcu_tcp_poll(1, 1);
    now += 1000U;
    handoff_mcu_rk_poll();
    uint64_t current = heartbeat_sequence;
    CHECK(!jump_attempts && current > 2U);

    /* 坏CRC、旧/未来心跳不放行；本轮有效心跳也必须等业务和UART空闲。 */
    uart_input_length = (uint16_t)tlv_pack_heartbeat(current, uart_input, sizeof(uart_input));
    uart_input[TLV_FRAME_MIN - 2U] ^= 1U;
    handoff_mcu_rk_poll();
    receive_heartbeat(current - 1U);
    receive_heartbeat(current + 1U);
    CHECK(!jump_attempts);
    uart_idle = 0;
    receive_heartbeat(current);
    CHECK(!jump_attempts);
    uart_idle = 1; power_busy = 1;
    handoff_mcu_rk_poll(); CHECK(!jump_attempts);
    power_busy = 0; test_busy = 1;
    handoff_mcu_rk_poll(); CHECK(!jump_attempts);
    test_busy = 0; rk_idle = 0;
    handoff_mcu_rk_poll(); CHECK(!jump_attempts);
    rk_idle = 1;
    handoff_mcu_rk_poll();
    CHECK(jump_attempts == 1 && jump_sequence == current + 1U);
    CHECK(last_tlv_length == sizeof(golden) && s_boot_phase == HANDOFF_BOOT_WAIT_TX);
    CHECK(!handoff_mcu_power_allowed(0) && handoff_mcu_power_allowed(1));
    CHECK(tlv_pack_cmd(HANDOFF_RK_CMD_CONTINUE_BOOT, NULL, 0U, jump_sequence,
                      expected, sizeof(expected)) == sizeof(golden));
    CHECK(memcmp(last_tlv, expected, sizeof(golden)) == 0);

    /* 入队不等于TC；提前到达的HELLO留在RX，TC之后才响应。 */
    sn_frame_t hello = {.op = SN_HELLO, .session = 101U, .sequence = 1U};
    uart_input_length = sn_pack(&hello, uart_input, sizeof(uart_input));
    int sends = tlv_sends;
    uart_idle = 0;
    now += 5000U;
    handoff_mcu_rk_poll();
    CHECK(uart_input_length && !s_state.session && tlv_sends == sends);
    uart_idle = 1;
    handoff_mcu_rk_poll();
    CHECK(!uart_input_length && s_boot_phase == HANDOFF_BOOT_EDK);
    CHECK(s_state.session == 101U && !s_active && s_response.status == SN_NOT_READY);
    handoff_mcu_tcp_poll(1, 1);
    CHECK(s_state.phase == HANDOFF_ARMED && !pending_callback && !s_saved);
    now += 10000U;
    handoff_mcu_rk_poll();
    CHECK(jump_attempts == 1 && tlv_sends == sends);
    end_boot_test();

    /* 跳转发送失败保持MCU地址，不补发、不授权网络；明确掉电后才能重开一轮。 */
    arm_boot_test();
    power_state = PWR_CTRL_STATE_ON;
    handoff_mcu_tcp_poll(0, 1);
    now += 1000U; handoff_mcu_rk_poll();
    uart_fail = 1;
    receive_heartbeat(heartbeat_sequence);
    CHECK(jump_attempts == 2 && s_boot_phase == HANDOFF_BOOT_FAILED);
    uart_fail = 0; sends = tlv_sends;
    now += 10000U; handoff_mcu_rk_poll();
    CHECK(jump_attempts == 2 && tlv_sends == sends && !s_saved && !s_state.session);
    end_boot_test();

    /* 自动引导仍兼容：坏HELLO不切换，合法HELLO直接接EDK，不发003E。 */
    arm_boot_test();
    power_state = PWR_CTRL_STATE_ON;
    handoff_mcu_tcp_poll(1, 1);
    hello.status = SN_INVALID;
    uart_input_length = sn_pack(&hello, uart_input, sizeof(uart_input));
    handoff_mcu_rk_poll();
    CHECK(s_boot_phase == HANDOFF_BOOT_PROBE && !s_state.session);
    hello.status = 0U;
    uart_input_length = sn_pack(&hello, uart_input, sizeof(uart_input));
    sends = tlv_sends;
    handoff_mcu_rk_poll();
    CHECK(s_boot_phase == HANDOFF_BOOT_EDK && s_state.session == hello.session);
    CHECK(tlv_sends == sends && jump_attempts == 2);
    end_boot_test();
    message("PASS: SPL 003E golden frame, live Seq, readiness, TC, no retry and auto EDK\n");
}

static void test_receive_wait(void)
{
    handoff_boot_phase_t saved_phase = s_boot_phase;
    uint8_t saved_rate = s_rate_pending, saved_reset = s_reset_stream;
    s_rate_pending = s_reset_stream = 0U;
    s_boot_phase = HANDOFF_BOOT_EDK;
    handoff_mcu_rk_wait();
    CHECK(rx_waits == 1U && idle_waits == 0U);
    /* 控制期可能留有未消费RX；必须按节拍让出CPU，不被队列中的旧块反复唤醒。 */
    s_rate_pending = 1U;
    handoff_mcu_rk_wait();
    s_rate_pending = 0U; s_reset_stream = 1U;
    handoff_mcu_rk_wait();
    s_reset_stream = 0U;
    s_boot_phase = HANDOFF_BOOT_PROBE; handoff_mcu_rk_wait();
    s_boot_phase = HANDOFF_BOOT_WAIT_TX; handoff_mcu_rk_wait();
    s_boot_phase = HANDOFF_BOOT_FAILED; handoff_mcu_rk_wait();
    CHECK(rx_waits == 1U && idle_waits == 5U);
    s_boot_phase = saved_phase;
    s_rate_pending = saved_rate; s_reset_stream = saved_reset;
    message("PASS: MCU data RX wake and bounded control/fault wait\n");
}

void test_main(void)
{
    struct netif netif = {.hwaddr = {2, 3, 4, 5, 6, 7}, .flags = NETIF_FLAG_LINK_UP,
                         .linkoutput = link_output};
    IP4_ADDR(&netif.ip_addr, 192, 168, 1, 211);
    IP4_ADDR(&netif.netmask, 255, 255, 255, 0);
    IP4_ADDR(&netif.gw, 192, 168, 1, 1);
    ip4_addr_t original_ip = netif.ip_addr, original_mask = netif.netmask, original_gw = netif.gw;
    s_rx_queue = (QueueHandle_t)(uintptr_t)1U;
    s_tx_done = (SemaphoreHandle_t)(uintptr_t)2U;
    handoff_mcu_attach(&netif);
    CHECK(!handoff_slt_begin(100U)); /* 独占模式不能让正式SLT悄悄断开PC连接。 */
    CHECK(s_mac[0] == 6 && netif.hwaddr[0] == 2);

    /* 只登记命令；忙或未掉电时TCP拒绝，原OFF不会撤销刚arm的状态。 */
    power_state = PWR_CTRL_STATE_ON;
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_RK));
    CHECK(!handoff_mcu_busy());
    handoff_mcu_tcp_poll(0, 1);
    CHECK(!handoff_mcu_busy());
    power_state = PWR_CTRL_STATE_OFF; test_busy = 1;
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_RK));
    handoff_mcu_tcp_poll(0, 1);
    CHECK(!handoff_mcu_busy());
    test_busy = 0;
    log_result = 0;
    last_log[0] = '\0';
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_RK));
    handoff_mcu_tcp_poll(0, 1);
    CHECK(s_state.phase == HANDOFF_ARMED && handoff_mcu_listen_allowed());
    CHECK(s_report_length && last_log[0] == '\0');
    int blocked_calls = log_calls;
    handoff_mcu_tcp_poll(0, 1);
    CHECK(log_calls == blocked_calls + 1 && s_report_length);
    log_result = 1;
    handoff_mcu_tcp_poll(0, 1);
    CHECK(!s_report_length);
    CHECK(strstr(last_log, "phase=armed ip=192.168.1.211 rkMAC=06:03:04:05:06:07"));
    /* 来源切换只取消待发快照，恢复MCU模式后不能回放旧状态。 */
    log_result = -1;
    report("state");
    handoff_mcu_tcp_poll(0, 1);
    CHECK(!s_report_length);
    int cancelled_calls = log_calls;
    log_result = 1;
    handoff_mcu_tcp_poll(0, 1);
    CHECK(log_calls == cancelled_calls);
    CHECK(handoff_mcu_rk_poll() == 1);
    handoff_mcu_tcp_poll(0, 1);
    CHECK(s_state.phase == HANDOFF_ARMED && netif.ip_addr.addr == original_ip.addr);

    /* 不获许可的HELLO/RESET不能启用桥；PC仍连接或UART未发完都不能撤IP。 */
    power_state = PWR_CTRL_STATE_ON;
    CHECK(transact(SN_HELLO, 17U) == SN_NOT_READY);
    CHECK(transact(SN_RESET, 17U) == SN_NOT_READY && !s_active);
    CHECK(transact(SN_RELEASE, 17U) == SN_INVALID);
    CHECK(transact(SN_HELLO, 18U) == SN_INVALID && s_state.session == 17U);
    handoff_mcu_tcp_poll(1, 1);
    CHECK(s_state.phase == HANDOFF_ARMED && !pending_callback);
    uart_idle = 0;
    handoff_mcu_tcp_poll(0, 1);
    CHECK(s_state.phase == HANDOFF_ARMED);
    uart_idle = 1;
    handoff_mcu_tcp_poll(0, 1);
    CHECK(s_state.phase == HANDOFF_WITHDRAWING && !pending_callback && !handoff_mcu_listen_allowed());
    callback_reject = 1;
    handoff_mcu_tcp_poll(0, 0);
    CHECK(!s_network_pending && netif.ip_addr.addr == original_ip.addr);
    callback_reject = 0; eth_idle = 0;
    handoff_mcu_tcp_poll(0, 0); run_callback();
    CHECK(netif.ip_addr.addr == original_ip.addr);
    handoff_mcu_tcp_poll(0, 0); eth_idle = 1; run_callback();
    CHECK(netif.ip_addr.addr == 0U && s_saved);
    handoff_mcu_tcp_poll(0, 0);
    CHECK(s_state.phase == HANDOFF_OWNED && !s_active && !s_state.authorized);
    CHECK(transact(SN_HELLO, 17U) == SN_OK);
    CHECK(s_active && s_response.length == 21U);
    CHECK(memcmp(s_response.data + 9, &original_ip.addr, 4U) == 0);
    CHECK(memcmp(s_response.data + 13, &original_mask.addr, 4U) == 0);
    CHECK(memcmp(s_response.data + 17, &original_gw.addr, 4U) == 0);
    test_baud();

    /* STOP/RESET只影响网卡，不能恢复IP；异session不能夺取已经授权的会话。 */
    CHECK(transact(SN_STOP, 17U) == SN_OK && !s_active);
    CHECK(s_state.phase == HANDOFF_OWNED && netif.ip_addr.addr == 0U);
    CHECK(transact(SN_RESET, 17U) == SN_OK && s_active);
    CHECK(transact(SN_RELEASE, 18U) == SN_INVALID && s_active);
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_MCU));
    handoff_mcu_tcp_poll(0, 0);
    CHECK(s_state.phase == HANDOFF_OWNED);

    /* 实际SEND路径：错误源IP不入tcpip；超时保留缓冲且同事务不会重发。 */
    sn_frame_t send = {.op = SN_SEND, .session = 17U, .length = 42U};
    memset(send.data, 255, 6U);
    memcpy(send.data + 6U, s_mac, 6U);
    send.data[12] = 8U; send.data[13] = 6U;
    send.data[15] = 1U; send.data[16] = 8U;
    send.data[18] = 6U; send.data[19] = 4U; send.data[21] = 1U;
    memcpy(send.data + 22U, s_mac, 6U);
    send.data[28] = 1U;
    CHECK(transmit(&send) == SN_INVALID && !pending_callback);
    memcpy(send.data + 28U, &original_ip.addr, 4U);
    send.sequence = s_server.request.sequence + 1U;
    sn_frame_t send_response;
    CHECK(sn_server_handle(&s_server, &send, &send_response, execute, NULL) == 1);
    CHECK(send_response.status == SN_IO_ERROR && s_tx_pending && pending_callback);
    int before_callbacks = callback_count;
    CHECK(sn_server_handle(&s_server, &send, &send_response, execute, NULL) == 0);
    CHECK(send_response.status == SN_IO_ERROR && callback_count == before_callbacks);
    struct pbuf received = {.payload = send.data, .len = 42U, .tot_len = 42U};
    CHECK(handoff_mcu_input(&received) == 0 && rx_count == 1);

    /* RELEASE立即停转发；tcpip旧包与DMA任一未排空都只能NOT_READY。 */
    CHECK(transact(SN_RELEASE, 17U) == SN_NOT_READY && !s_active);
    CHECK(handoff_mcu_input(&received) == 0 && rx_count == 0);
    run_callback();
    CHECK(!s_tx_pending && tx_count == 0);
    s_tx_pending = 1U;
    handoff_mcu_tcp_poll(0, 0); run_callback();
    CHECK(!s_network_ok && netif.ip_addr.addr == 0U);
    s_tx_pending = 0U; eth_idle = 0;
    handoff_mcu_tcp_poll(0, 0); run_callback();
    CHECK(!s_network_ok && netif.ip_addr.addr == 0U);
    CHECK(transact(SN_RELEASE, 17U) == SN_NOT_READY);
    eth_idle = 1;
    handoff_mcu_tcp_poll(0, 0); run_callback();
    CHECK(netif.ip_addr.addr == original_ip.addr && arp_count == 1);
    handoff_mcu_tcp_poll(0, 0);
    CHECK(s_state.phase == HANDOFF_RESTORED && handoff_mcu_listen_allowed());
    CHECK(transact(SN_RELEASE, 17U) == SN_NOT_READY);
    handoff_mcu_tcp_poll(0, 1);
    CHECK(s_state.phase == HANDOFF_EDK_IDLE && handoff_mcu_busy());
    CHECK(transact(SN_RELEASE, 17U) == SN_OK);
    sn_frame_t replay;
    CHECK(sn_server_handle(&s_server, &s_server.request, &replay, execute, NULL) == 0 && replay.status == SN_OK);
    CHECK(transact(SN_RESET, 17U) == SN_NOT_READY && !s_active);
    CHECK(transact(SN_HELLO, 18U) == SN_INVALID);
    CHECK(!handoff_mcu_power_allowed(0) && handoff_mcu_power_allowed(1));

    /* 无超时抢IP/假完成；只有真实OFF才清会话，回TLV后正常FUNC必须另行冷启动。 */
    now += 1000000U;
    handoff_mcu_tcp_poll(0, 1);
    CHECK(s_state.phase == HANDOFF_EDK_IDLE);
    power_state = PWR_CTRL_STATE_OFF;
    eth_idle = 0;
    handoff_mcu_tcp_poll(1, 1);
    run_callback();
    CHECK(s_state.phase == HANDOFF_EDK_IDLE && handoff_mcu_listen_allowed());
    eth_idle = 1;
    handoff_mcu_tcp_poll(1, 1); run_callback();
    handoff_mcu_tcp_poll(1, 1);
    CHECK(handoff_mcu_busy() && !handoff_mcu_power_allowed(0));
    CHECK(handoff_mcu_rk_poll() == 0 && !handoff_mcu_busy() && !s_server.valid);
    CHECK(actual_baud == SN_BOOT_BAUD && s_uart_baud == SN_BOOT_BAUD && baud_changes == 4U);
    CHECK(netif.ip_addr.addr == original_ip.addr);

    /* 已授权但未RELEASE时确定掉电，仍必须走DMA排空与恢复，不跳过回收步骤。 */
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_RK));
    handoff_mcu_tcp_poll(0, 1); handoff_mcu_rk_poll();
    power_state = PWR_CTRL_STATE_ON;
    CHECK(transact(SN_HELLO, 99U) == SN_NOT_READY);
    handoff_mcu_tcp_poll(0, 1); handoff_mcu_tcp_poll(0, 0); run_callback();
    handoff_mcu_tcp_poll(0, 0);
    CHECK(transact(SN_HELLO, 99U) == SN_OK && netif.ip_addr.addr == 0U);
    /* HAL已改速但RX重启失败：故障期间仍可下电，OFF后未恢复前不得开始下一轮。 */
    s_boot_phase = HANDOFF_BOOT_EDK;
    sn_frame_t rate = {.op = SN_HELLO, .session = 99U,
        .sequence = s_server.request.sequence + 1U, .length = SN_RATE_REQUEST_SIZE};
    for (unsigned i = 0U; i < SN_RATE_REQUEST_SIZE; ++i)
        rate.data[i] = (uint8_t)(SN_DATA_BAUD >> (i * 8U));
    receive_serial(&rate);
    baud_fail = 1;
    handoff_mcu_rk_poll();
    CHECK(s_boot_phase == HANDOFF_BOOT_FAILED && s_baud_unknown && actual_baud == SN_DATA_BAUD);
    CHECK(!handoff_mcu_power_allowed(0) && handoff_mcu_power_allowed(1));
    power_state = PWR_CTRL_STATE_OFF;
    handoff_mcu_tcp_poll(0, 0);
    CHECK(s_state.phase == HANDOFF_RELEASING && !s_active);
    run_callback();
    handoff_mcu_tcp_poll(0, 0); handoff_mcu_tcp_poll(0, 1);
    CHECK(netif.ip_addr.addr == original_ip.addr && handoff_mcu_busy());
    CHECK(handoff_mcu_rk_poll() == 1 && handoff_mcu_busy() && !handoff_mcu_power_allowed(0));
    baud_fail = 0;
    CHECK(handoff_mcu_rk_poll() == 0 && !handoff_mcu_busy() && actual_baud == SN_BOOT_BAUD);

    /* arm后明确OFF取消也合法，不需要伪造一次ON；从未授权不能RELEASE。 */
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_RK));
    handoff_mcu_tcp_poll(0, 1);
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_MCU));
    handoff_mcu_tcp_poll(0, 1);
    handoff_mcu_tcp_poll(0, 0); run_callback();
    handoff_mcu_tcp_poll(0, 0); handoff_mcu_tcp_poll(0, 1);
    CHECK(handoff_mcu_rk_poll() == 0 && !handoff_mcu_busy() && !s_state.authorized);
    CHECK(callback_count > 4 && tx_count == 0);
    test_spl_boot();
    test_receive_wait();
    message("PASS: MCU handoff authorization, listener order, drain, replay and OFF recovery\n");
    finish(0U);
}
