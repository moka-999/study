/* 实际TCP任务的离线连接生命周期回归；socket为契约桩，不代替lwIP/PHY实板验收。 */
#include "port.h"
#include "pc_command.h"
#include "pc_dispatch.h"
#include <setjmp.h>
#include <string.h>
#include "task_under_test.h"

#define STR1(x) #x
#define STR(x) STR1(x)
#define CHECK(x) do { if (!(x)) fail("FAIL " STR(__LINE__) ": " #x "\n"); } while (0)
enum { NORMAL, RECV_ERROR, SEND_ERROR, PARTIAL_TIMEOUT, DISPATCH_ERROR, HANDOFF, LISTEN_RECOVERY };
enum { LISTENER = 3, CLIENT = 4, FRAME_SIZE = 20, FAILED_SELECTS = 52 };
static int scenario, listening, connected, pending, step, accepted, closed, dispatched, sent;
static int listener_closes, listener_opens, poll_active, disconnect_calls, trace_connections;
static int select_failures, recovery_closes, idle_polls, listen_error_logs;
static uint32_t tick;
static size_t send_offset;
#if defined(PC_COMMAND_CHIP_INFO)
static unsigned send_notifications;
#endif
static jmp_buf exit_task;
static const uint8_t request[FRAME_SIZE] = {0,0,0,20,0,1,0,8,0,0,6,1,0,0,0,9,0,0,'{','}'};
int test_errno;
struct netif gnetif = {{1U}};

static void message(const char *text)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = text;
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
static _Noreturn void fail(const char *text) { message(text); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }
uint32_t xTaskGetTickCount(void) { return tick; }
void vTaskDelay(uint32_t ms)
{
    tick += ms;
    if (scenario == LISTEN_RECOVERY && !gnetif.ip_addr.addr && tick >= 21000U) {
        CHECK(tcp_diag[1] == 0xBEEF00E1U && !listener_opens);
        gnetif.ip_addr.addr = 1U;
    }
}
void vTaskDelete(void *task) { (void)task; fail("FAIL unexpected task deletion\n"); }
void task_watchdog_progress(int task) { CHECK(task == TASK_WATCHDOG_TCP); }
void led_on(int led) { CHECK(led == LED_CONNECT); }
void led_off(int led) { CHECK(led == LED_CONNECT); }
void b2_service_poll(int active)
{
    CHECK(active == connected);
    if (!active) ++idle_polls;
    if (scenario == HANDOFF && closed == 1 && !active) {
        CHECK(!listening && listener_opens == 1);
        longjmp(exit_task, 1);
    }
}
void handoff_mcu_tcp_poll(int active, int listener_open)
{
    CHECK(active == connected && listener_open == listening);
    if (active) { CHECK(!listener_open); ++poll_active; }
}
int handoff_mcu_listen_allowed(void) { return scenario != HANDOFF || closed == 0; }
uint16_t htons(uint16_t n) { return (uint16_t)((n << 8) | (n >> 8)); }
uint16_t ntohs(uint16_t n) { return htons(n); }
int socket(int family, int type, int protocol)
{
    CHECK(family == AF_INET && type == SOCK_STREAM && protocol == 0);
    CHECK(!listening && !connected); ++listener_opens; return LISTENER;
}
int setsockopt(int fd, int level, int option, const void *value, socklen_t length)
{
    CHECK(level == SOL_SOCKET && value && length > 0);
    if (fd == LISTENER) CHECK(option == SO_REUSEADDR);
    else CHECK(fd == CLIENT && connected && !listening);
    return 0;
}
int bind(int fd, const struct sockaddr *address, socklen_t length)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)address;
    CHECK(fd == LISTENER && length == sizeof(*in) && ntohs(in->sin_port) == 8888U);
    return 0;
}
int listen(int fd, int backlog)
{
    CHECK(fd == LISTENER && !connected && backlog == 1); listening = 1; return 0;
}
int select(int n, fd_set *readers, fd_set *writers, fd_set *errors, struct timeval *wait)
{
    CHECK(listening && !connected && n == LISTENER + 1 && readers->bits == (1U << LISTENER));
    CHECK(!writers && !errors && wait);
    if (scenario == LISTEN_RECOVERY && select_failures < FAILED_SELECTS) {
        ++select_failures;
        test_errno = ENOMEM;
        return -1;
    }
    if (closed == (scenario == NORMAL ? 2 : 1)) longjmp(exit_task, 1);
    return 1;
}
int accept(int fd, struct sockaddr *peer, socklen_t *size)
{
    CHECK(fd == LISTENER && listening && !connected);
    /* 同时握手的第二连接仅进入待accept队列；关闭监听时应交给lwIP回收。 */
    pending = 5; connected = 1; ++accepted; step = 0; send_offset = 0;
    if (peer) {
        struct sockaddr_in *in = (struct sockaddr_in *)peer;
        CHECK(size && *size >= sizeof(*in)); memset(in, 0, sizeof(*in));
        in->sin_family = AF_INET; in->sin_port = htons(59178);
        const uint8_t address[4] = {192,168,1,111}; memcpy(&in->sin_addr.s_addr, address, sizeof(address));
        *size = sizeof(*in);
    }
    return CLIENT;
}
int close(int fd)
{
    if (fd == LISTENER) {
        CHECK(listening);
        if (!connected) {
            CHECK(scenario == LISTEN_RECOVERY);
            if (select_failures == 1) { test_errno = ENOMEM; return -1; }
            ++recovery_closes;
        }
        listening = 0; pending = 0; ++listener_closes;
    } else {
        CHECK(fd == CLIENT && connected && !listening && !pending);
        CHECK(disconnect_calls == accepted); connected = 0; ++closed;
    }
    return 0;
}
int recv(int fd, void *buffer, size_t capacity, int flags)
{
    CHECK(fd == CLIENT && connected && !listening && !pending && flags == 0 && capacity >= FRAME_SIZE);
    tick += 20; ++step;
    if (scenario == RECV_ERROR) { test_errno = ECONNRESET; return -1; }
    if (scenario == PARTIAL_TIMEOUT) {
        if (step == 1) { memcpy(buffer, request, 8); return 8; }
        tick += 600; test_errno = EAGAIN; return -1;
    }
    if (step == 1 || (scenario == NORMAL && step == 3)) {
        memcpy(buffer, request, FRAME_SIZE); return FRAME_SIZE;
    }
    if (scenario == NORMAL && step == 2) { test_errno = EAGAIN; return -1; }
    return 0;
}
int send(int fd, const void *data, size_t length, int flags)
{
    CHECK(fd == CLIENT && connected && !listening && !pending && flags == 0);
    if (scenario == SEND_ERROR && send_offset) { test_errno = ECONNRESET; return -1; }
    const uint8_t *bytes = data;
    CHECK(length == FRAME_SIZE - send_offset);
    if (!send_offset) CHECK(bytes[11] == 2 && bytes[15] == (uint8_t)(sent % 2 + 1));
    /* 人为拆成两次send，确保监听关闭不会破坏已接受连接的正常短发送。 */
    size_t written = send_offset == 0 ? 7 : length;
    send_offset += written;
    if (send_offset == FRAME_SIZE) { send_offset = 0; ++sent; }
    return (int)written;
}
void pc_dispatch_poll(void) { test_errno = 0; }
#if defined(PC_COMMAND_CHIP_INFO)
void pc_dispatch_response_sent(uint16_t command)
{
    ++send_notifications;
    CHECK(command == 0x0602U && !send_offset && sent == (int)send_notifications);
}
#endif
int pc_dispatch_next_response(uint32_t sequence, pc_dispatch_response_t *response)
{ (void)sequence; (void)response; return 0; }
pc_dispatch_result_t pc_dispatch_handle(const pc_protocol_frame_t *frame, uint32_t sequence,
                                       pc_dispatch_response_t *response)
{
    CHECK(frame->command_id == 0x0601 && frame->json_length == 2);
    CHECK(memcmp(frame->json, "{}", 2) == 0); ++dispatched;
    if (scenario == DISPATCH_ERROR) return PC_DISPATCH_ERROR;
    memcpy(response->data, request, FRAME_SIZE); response->data[11] = 2;
    response->data[15] = (uint8_t)sequence; response->length = FRAME_SIZE;
#if defined(TEST_B1_TRACE)
    response->command_id = 0x0602;
#endif
    return PC_DISPATCH_RESPONSE_READY;
}
void pc_dispatch_disconnected(void) { ++disconnect_calls; }
#if defined(TEST_B1_TRACE)
void pc_trace_listen_error(int socket_error)
{
    CHECK(scenario == LISTEN_RECOVERY && socket_error == ENOMEM);
    ++listen_error_logs;
}
void pc_trace_connected(const uint8_t peer_ip[4], uint16_t port)
{
    const uint8_t address[4] = {192,168,1,111};
    CHECK(!listening && connected && memcmp(peer_ip, address, 4) == 0 && port == 59178);
    ++trace_connections;
}
void pc_trace_rx_error(int socket_error)
{
    CHECK(scenario == RECV_ERROR && socket_error == ECONNRESET);
}
void pc_trace_closed(const char *reason)
{
    const char *expected = scenario == RECV_ERROR ? "recvFailed" : scenario == SEND_ERROR ? "sendFailed" :
        scenario == PARTIAL_TIMEOUT ? "partialTimeout" : scenario == DISPATCH_ERROR ? "dispatchError" : "peerClosed";
    CHECK(strcmp(reason, expected) == 0);
}
void pc_trace_rx(const pc_protocol_frame_t *frame) { CHECK(frame->command_id == 0x0601); }
void pc_trace_tx(const pc_dispatch_response_t *response, uint32_t sequence, size_t queued, int error)
{
    CHECK(sequence > 0 && response->command_id == 0x0602);
    CHECK(scenario == SEND_ERROR ? queued == 7U && error == ECONNRESET : queued == FRAME_SIZE && error == 0);
}
#endif
static void run(int mode)
{
    scenario = mode; listening = connected = pending = step = accepted = closed = dispatched = sent = 0;
    listener_closes = listener_opens = poll_active = disconnect_calls = trace_connections = 0;
    select_failures = recovery_closes = idle_polls = listen_error_logs = 0;
    gnetif.ip_addr.addr = mode == LISTEN_RECOVERY ? 0U : 1U;
    tick = 0; send_offset = 0;
#if defined(PC_COMMAND_CHIP_INFO)
    send_notifications = 0U;
#endif
    if (setjmp(exit_task) == 0) StartTcpServerTask(NULL);
    CHECK(!connected && !pending && closed == accepted && listener_closes == accepted + recovery_closes);
    CHECK(accepted == (mode == NORMAL ? 2 : 1));
    if (mode == NORMAL) CHECK(dispatched == 4 && sent == 4);
    if (mode == RECV_ERROR || mode == PARTIAL_TIMEOUT) CHECK(dispatched == 0 && sent == 0);
    if (mode == SEND_ERROR || mode == DISPATCH_ERROR) CHECK(dispatched == 1 && sent == 0);
#if defined(PC_COMMAND_CHIP_INFO)
    CHECK(send_notifications == (unsigned)sent);
#endif
#if defined(TEST_B1_TRACE)
    CHECK(trace_connections == accepted && poll_active > 0);
    if (mode == LISTEN_RECOVERY) {
        CHECK(recovery_closes == FAILED_SELECTS - 1 && idle_polls >= FAILED_SELECTS);
        CHECK(listen_error_logs == 2 && dispatched == 1 && sent == 1);
    }
#endif
}
int test_main(void)
{
    for (int mode = NORMAL; mode <= DISPATCH_ERROR; ++mode) run(mode);
#if defined(TEST_B1_TRACE)
    run(HANDOFF);
    run(LISTEN_RECOVERY);
#endif
    message("PASS: TCP single-client task, listener lifecycle, peer, short send and error paths\n");
    finish(0U);
}
