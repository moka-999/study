/* USART1与TCP调试控制台共用命令执行，各自保存输入、来源和控制回复。 */
#include "log_console.h"
#include "net_console.h"
#include "handoff_mcu.h"
#include "result_led.h"
#include "uart_router.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LOG_SOURCE_MCU = 0, LOG_SOURCE_RK = 1 };
#define LOG_TX_CHUNK_SIZE 128U
#define LOG_COMMAND_CAPACITY 16U
static const char s_rk_escape[] = "log mcu";

typedef struct {
    uint32_t session; /* 串口固定1，网络为连接代数；0表示未连接。 */
    uint8_t source, status_pending, discard_line, rk_line_start, skip_lf;
    char command[LOG_COMMAND_CAPACITY];
    size_t command_length, rk_escape_length;
    const char *reply;
    char led_reply[96];
    uint8_t rk_pending[UART_RX_BUF_SIZE + sizeof(s_rk_escape) - 1U];
    size_t rk_pending_length;
} console_t;

static console_t s_serial, s_network;
static console_t *s_rk_owner, *s_status_owner, *s_power_owner;
static uint32_t s_status_session, s_power_session;
static uint8_t s_started;
static uint8_t s_site_enabled = 1U;
static log_console_power_request_t s_power_request;
static uint8_t s_power_taken, s_power_reply_ready;
static char s_power_reply[128];
static volatile uint32_t s_dropped_bytes;

static void count_drop(size_t bytes)
{
    s_dropped_bytes = bytes > UINT32_MAX - s_dropped_bytes ?
                      UINT32_MAX : s_dropped_bytes + (uint32_t)bytes;
}

/* 调用方持有短临界区；仅复制有界数据，不执行网络API或等待串口空间。 */
static int output(console_t *console, const void *data, size_t length, int drop)
{
    if (!console->session) return -1;
    if (console == &s_network)
        return net_console_write(console->session, data, length, drop);
    int sent = uart_send(UART_PORT_CONSOLE, (uint8_t *)data, (uint16_t)length) == IF_OK;
    if (!sent && drop) count_drop(length);
    return sent;
}

static void flush_reply(console_t *console)
{
    if (s_power_reply_ready && s_power_owner == console) {
        if (!console->session || console->session != s_power_session) {
            s_power_reply_ready = 0U;
            s_power_request = LOG_CONSOLE_POWER_NONE;
        } else if (!console->reply) console->reply = s_power_reply;
    }
    if (console->reply && output(console, console->reply, strlen(console->reply), 0) != 0) {
        if (console->reply == s_power_reply) {
            s_power_reply_ready = 0U;
            s_power_request = LOG_CONSOLE_POWER_NONE;
        }
        console->reply = NULL;
    }
}

static size_t write_source(uint8_t source, const uint8_t *data, size_t length, size_t chunk_limit)
{
    if (!s_started || !data || __get_IPSR() != 0U) return 0U;
    size_t serial_sent = 0U;
    int serial_failed = 0;
    for (size_t offset = 0U; offset < length;) {
        size_t chunk = length - offset;
        if (chunk > chunk_limit) chunk = chunk_limit;
        taskENTER_CRITICAL();
        if (s_serial.source == source && s_serial.reply) {
            count_drop(chunk);
            serial_sent += chunk;
        } else if (s_serial.source == source && !serial_failed) {
            if (output(&s_serial, data + offset, chunk, 1) > 0) serial_sent += chunk;
            else {
                count_drop(length - offset - chunk);
                serial_failed = 1;
            }
        } else if (s_serial.source != source) serial_sent += chunk;
        if (s_network.session && s_network.source == source)
            (void)output(&s_network, data + offset, chunk, 1);
        taskEXIT_CRITICAL();
        offset += chunk;
    }
    return serial_sent;
}

size_t log_console_write(const char *text, size_t length)
{
    return write_source(LOG_SOURCE_MCU, (const uint8_t *)text, length, LOG_TX_CHUNK_SIZE);
}

int log_console_mcu_selected(void)
{
    taskENTER_CRITICAL();
    int selected = s_started && (s_serial.source == LOG_SOURCE_MCU ||
        (s_network.session && s_network.source == LOG_SOURCE_MCU));
    taskEXIT_CRITICAL();
    return selected;
}

void log_console_write_record(const char *text, size_t length)
{
    if (length && length <= LOG_CONSOLE_RECORD_SIZE)
        (void)write_source(LOG_SOURCE_MCU, (const uint8_t *)text, length, LOG_CONSOLE_RECORD_SIZE);
}

uint32_t log_console_dropped_bytes(void) { return s_dropped_bytes; }

void log_console_start(void)
{
    static const char banner[] = "B1 gateway software ready; power remains OFF\r\n";
    taskENTER_CRITICAL();
    memset(&s_serial, 0, sizeof(s_serial));
    memset(&s_network, 0, sizeof(s_network));
    s_serial.session = 1U;
    s_serial.source = B1_LOG_DEFAULT_SOURCE;
    s_serial.rk_line_start = 1U;
    s_rk_owner = s_serial.source == LOG_SOURCE_RK ? &s_serial : NULL;
    s_status_owner = &s_serial;
    s_status_session = s_serial.session;
    s_power_request = LOG_CONSOLE_POWER_NONE;
    s_power_taken = s_power_reply_ready = 0U;
    s_started = 1U;
    taskEXIT_CRITICAL();
    (void)log_console_write(banner, sizeof(banner) - 1U);
}

static const char *apply_led_command(console_t *console, const char *command)
{
    static const char usage[] = "\r\n[led] use: led 0|1|2|all 00|01|10|11|off|pass|fail|normal|hot\r\n";
    static const struct {
        const char *name;
        result_led_state_t state;
    } modes[] = {
        {"off", RESULT_LED_OFF}, {"pass", RESULT_LED_PASS},
        {"fail", RESULT_LED_FAIL}, {"normal", RESULT_LED_TEST_NORMAL},
        {"hot", RESULT_LED_TEST_HOT},
        /* 左位控制FAIL引脚，右位控制PASS引脚；0低、1高，四种组合均不闪烁。 */
        {"00", RESULT_LED_OFF}, {"01", RESULT_LED_PASS},
        {"10", RESULT_LED_FAIL}, {"11", RESULT_LED_DEBUG_BOTH_ON}
    };
    const char *value = command + 4U;
    uint32_t target;
    if (strncmp(value, "all ", 4U) == 0) {
        target = RESULT_LED_TARGET_COUNT;
        value += 4U;
    } else if (value[0] >= '0' && value[0] <= '2' && value[1] == ' ') {
        target = (uint32_t)(value[0] - '0');
        value += 2U;
    } else return usage;

    for (size_t i = 0U; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        if (strcmp(value, modes[i].name) != 0) continue;
        result_led_state_t states[RESULT_LED_TARGET_COUNT] = {RESULT_LED_OFF};
        for (uint32_t group = 0U; group < RESULT_LED_TARGET_COUNT; ++group)
            if (target == RESULT_LED_TARGET_COUNT || target == group)
                states[group] = modes[i].state;
        if (result_led_set_all(states) != HAL_OK)
            return "\r\n[led] failed: driver not ready\r\n";
        (void)snprintf(console->led_reply, sizeof(console->led_reply),
                       "\r\n[led] states=%u,%u,%u; 0=off 1=pass 2=fail 3=normal 4=hot 5=both\r\n",
                       (unsigned)states[0], (unsigned)states[1], (unsigned)states[2]);
        return console->led_reply;
    }
    return usage;
}

static void finish_command(console_t *console)
{
    if (console->command_length == 0U && !console->discard_line) return; /* 忽略空行及CRLF的LF。 */
    console->command[console->command_length] = '\0';
    taskENTER_CRITICAL();
    if (console == &s_network && console->session != net_console_session()) {
        console->command_length = 0U;
        taskEXIT_CRITICAL();
        return;
    }
    if (!console->discard_line && strcmp(console->command, "log mcu") == 0) {
        console->source = LOG_SOURCE_MCU;
        if (s_rk_owner == console && !console->rk_pending_length) s_rk_owner = NULL;
        console->reply = "\r\n[log] source=mcu\r\n";
    } else if (!console->discard_line && strcmp(console->command, "log rk") == 0) {
        console->source = LOG_SOURCE_RK;
        console->rk_escape_length = 0U;
        console->rk_line_start = 1U;
        console->status_pending = 0U;
        if (s_status_owner == console) s_status_session = 0U;
        if (!s_rk_owner) s_rk_owner = console;
        console->reply = s_rk_owner == console ? "\r\n[log] source=rk\r\n" :
            "\r\n[log] source=rk read-only; other console owns input\r\n";
    } else if (!console->discard_line && strcmp(console->command, "status") == 0) {
        if (console->source == LOG_SOURCE_MCU) {
            console->status_pending = 1U; /* 合并重复查询；状态由默认任务另行读取。 */
            console->reply = "\r\n[log] status queued\r\n";
        } else {
            console->reply = "\r\n[log] select MCU first: log mcu\r\n";
        }
    } else if (!s_site_enabled) {
        console->reply = "\r\n[log] siteDisabled\r\n";
    } else if (!console->discard_line &&
               (strcmp(console->command, "start rk") == 0 || strcmp(console->command, "end rk") == 0)) {
        /* 只入单槽请求，电源编排由唯一TCP上下文处理；此处仍在短临界区内。 */
        if (s_power_request != LOG_CONSOLE_POWER_NONE) console->reply = "\r\n[rk power] busy\r\n";
        else {
            s_power_request = console->command[0] == 's' ? LOG_CONSOLE_POWER_START_RK : LOG_CONSOLE_POWER_END_RK;
            s_power_taken = 0U;
            s_power_owner = console;
            s_power_session = console->session;
            console->reply = s_power_request == LOG_CONSOLE_POWER_START_RK ?
                "\r\n[rk power] start rk queued\r\n" : "\r\n[rk power] end rk queued\r\n";
        }
    } else if (!console->discard_line &&
               (strcmp(console->command, "net rk") == 0 || strcmp(console->command, "net mcu") == 0)) {
        handoff_command_t request = strcmp(console->command, "net rk") == 0 ? HANDOFF_COMMAND_RK : HANDOFF_COMMAND_MCU;
        console->reply = handoff_mcu_console_request(request) ?
            "\r\n[handoff] command queued\r\n" : "\r\n[handoff] command busy\r\n";
    } else if (!console->discard_line && strncmp(console->command, "led ", 4U) == 0) {
        console->reply = apply_led_command(console, console->command);
    } else {
        console->reply = "\r\n[log] use: log mcu | log rk | status | start rk | end rk | net rk | net mcu | led 0|1|2|all 00|01|10|11|off|pass|fail|normal|hot\r\n";
    }
    /* 确认串排在既有TX尾部；队列满则下一轮重试，期间不加入新日志。
     * 命令洪泛时只保留最新控制回复，实际选择以最后一条有效命令为准。 */
    flush_reply(console);
    taskEXIT_CRITICAL();
    console->command_length = 0U;
    console->discard_line = 0U;
}

/* 发送环整块接纳；满时保留原数据，下次推进不重复已接纳的字符。 */
static int flush_rk_input(console_t *console)
{
    if (console == &s_network && console->session != net_console_session()) {
        console->rk_pending_length = 0U;
        if (s_rk_owner == console) s_rk_owner = NULL;
        return 0;
    }
    if (console->rk_pending_length != 0U &&
        uart_send(UART_PORT_RK_DEBUG, console->rk_pending,
                  (uint16_t)console->rk_pending_length) != IF_OK) return 0;
    console->rk_pending_length = 0U;
    if (s_rk_owner == console && console->source != LOG_SOURCE_RK) s_rk_owner = NULL;
    return 1;
}

static void forward_rk_byte(console_t *console, uint8_t byte)
{
    if (console->rk_line_start) {
        if (byte == 0x03U) {
            /* Ctrl+C取消尚未发出的退出前缀，并立即交给RK中断当前输入。 */
            console->rk_escape_length = 0U;
        } else if ((byte == 0x08U || byte == 0x7FU) && console->rk_escape_length != 0U) {
            --console->rk_escape_length;
            return;
        } else if ((byte == '\r' || byte == '\n') &&
                   console->rk_escape_length == sizeof(s_rk_escape) - 1U) {
            memcpy(console->command, s_rk_escape, sizeof(s_rk_escape));
            console->command_length = sizeof(s_rk_escape) - 1U;
            console->rk_escape_length = 0U;
            finish_command(console);
            console->skip_lf = byte == '\r';
            return;
        } else if (console->rk_escape_length < sizeof(s_rk_escape) - 1U &&
                   byte == (uint8_t)s_rk_escape[console->rk_escape_length]) {
            ++console->rk_escape_length;
            return;
        } else {
            if (s_rk_owner == console) {
                memcpy(console->rk_pending + console->rk_pending_length, s_rk_escape, console->rk_escape_length);
                console->rk_pending_length += console->rk_escape_length;
            }
            console->rk_escape_length = 0U;
        }
    }
    if (s_rk_owner == console) console->rk_pending[console->rk_pending_length++] = byte;
    console->rk_line_start = byte == '\r' || byte == '\n' || byte == 0x03U;
}

static void consume_console_byte(console_t *console, uint8_t byte)
{
    if (console->skip_lf) {
        console->skip_lf = 0U;
        if (byte == '\n') return; /* 本地切换行的CRLF不能把LF泄漏给RK。 */
    }
    if (console->discard_line) {
        if (byte == '\r' || byte == '\n') {
            if (console->source == LOG_SOURCE_MCU) finish_command(console);
            console->discard_line = 0U;
            console->rk_line_start = 1U;
            console->skip_lf = byte == '\r';
        }
        return;
    }
    if (console->source == LOG_SOURCE_RK) {
        forward_rk_byte(console, byte);
    } else if (byte == '\r' || byte == '\n') {
        finish_command(console);
        console->skip_lf = byte == '\r';
    } else if (byte < 0x20U || byte > 0x7EU ||
               console->command_length >= sizeof(console->command) - 1U) {
        console->discard_line = 1U;
    } else {
        console->command[console->command_length++] = (char)byte;
    }
}

/* 串口和网络输入都只由默认任务解析，不能在lwIP回调里执行控制命令。 */
static void process_input(console_t *console, uint8_t *buffer)
{
    if (!console->session || !flush_rk_input(console)) return;
    int length = console == &s_serial ? uart_poll_rx(UART_PORT_CONSOLE, buffer, UART_RX_BUF_SIZE) :
        (int)net_console_read(console->session, buffer, UART_RX_BUF_SIZE);
    if (length == UART_RX_GAP) {
        console->command_length = console->rk_escape_length = 0U;
        console->skip_lf = 0U;
        console->discard_line = 1U;
        taskENTER_CRITICAL();
        console->reply = "\r\n[log] input lost; discard until newline\r\n";
        taskEXIT_CRITICAL();
    } else {
        for (int index = 0; index < length; ++index) {
            /* 断线或输入溢出后的旧字节不能执行，也不能作用于重连客户端。 */
            if (console == &s_network && net_console_session() != console->session) break;
            consume_console_byte(console, buffer[index]);
        }
        (void)flush_rk_input(console);
    }
}

void log_console_process(void)
{
    uint8_t buffer[UART_RX_BUF_SIZE];
    if (!s_started) return;
    uint32_t session = net_console_session();
    taskENTER_CRITICAL();
    if (session != s_network.session) {
        if (s_rk_owner == &s_network) s_rk_owner = NULL;
        memset(&s_network, 0, sizeof(s_network));
        s_network.session = session;
        s_network.rk_line_start = 1U;
        if (session) s_network.reply = "\r\n[net console] connected; source=mcu; type status or log rk\r\n";
    }
    flush_reply(&s_serial);
    flush_reply(&s_network);
    taskEXIT_CRITICAL();
    process_input(&s_serial, buffer);
    process_input(&s_network, buffer);
    taskENTER_CRITICAL();
    flush_reply(&s_serial);
    flush_reply(&s_network);
    taskEXIT_CRITICAL();
    /* 两个出口均未看RK时仍消费其日志，避免积压和阻塞DMA恢复。 */
    int length = uart_poll_rx(UART_PORT_RK_DEBUG, buffer, sizeof(buffer));
    if (length > 0) (void)write_source(LOG_SOURCE_RK, buffer, (size_t)length, LOG_TX_CHUNK_SIZE);
}

int log_console_take_status_request(void)
{
    taskENTER_CRITICAL();
    console_t *console = s_serial.status_pending ? &s_serial : &s_network;
    int requested = s_started && console->session && console->source == LOG_SOURCE_MCU &&
                    console->status_pending && !console->reply;
    if (requested) {
        console->status_pending = 0U;
        s_status_owner = console;
        s_status_session = console->session;
    }
    taskEXIT_CRITICAL();
    return requested;
}

log_console_power_request_t log_console_take_power_request(void)
{
    taskENTER_CRITICAL();
    log_console_power_request_t request = LOG_CONSOLE_POWER_NONE;
    if (s_started && s_power_request != LOG_CONSOLE_POWER_NONE && !s_power_taken &&
        (!s_power_owner->reply || s_power_owner->session != s_power_session)) {
        request = s_power_request;
        s_power_taken = 1U;
    }
    taskEXIT_CRITICAL();
    return request;
}

/* 调用者已经进入临界区；控制台拥塞时保留终态，直到原输出端能消费。 */
static void complete_power_locked(const char *error)
{
    if (s_power_request != LOG_CONSOLE_POWER_NONE && s_power_taken && !s_power_reply_ready) {
        const char *command = s_power_request == LOG_CONSOLE_POWER_START_RK ? "start rk" : "end rk";
        if (error) (void)snprintf(s_power_reply, sizeof(s_power_reply),
                                 "\r\n[rk power] %s failed: %.40s\r\n", command, error);
        else if (s_power_request == LOG_CONSOLE_POWER_START_RK)
            (void)snprintf(s_power_reply, sizeof(s_power_reply),
                           "\r\n[rk power] power ON; LPDDR4X 1800/1100/600mV; fspiM1; check RK boot log\r\n");
        else (void)snprintf(s_power_reply, sizeof(s_power_reply), "\r\n[rk power] power OFF\r\n");
        s_power_reply_ready = 1U;
    }
}

void log_console_complete_power_request(const char *error)
{
    taskENTER_CRITICAL();
    complete_power_locked(error);
    taskEXIT_CRITICAL();
}

void log_console_set_site_enabled(uint8_t enabled)
{
    taskENTER_CRITICAL();
    s_site_enabled = enabled;
    /* 命令可能在关站前已入槽，但“queued”仍堵在输出端；不能留到再开站才启动RK。 */
    if (!enabled && s_power_request != LOG_CONSOLE_POWER_NONE && !s_power_taken) {
        s_power_taken = 1U;
        complete_power_locked("cancelled");
    }
    taskEXIT_CRITICAL();
}

int log_console_write_status(const char *text, size_t length)
{
    if (!text || !length || length > LOG_TX_CHUNK_SIZE || __get_IPSR() != 0U) return -1;
    taskENTER_CRITICAL();
    console_t *console = s_status_owner;
    int result;
    if (!s_started || !console || !console->session || console->session != s_status_session ||
        console->source != LOG_SOURCE_MCU) result = -1;
    else if (console->reply) result = 0;
    else result = output(console, text, length, 0);
    taskEXIT_CRITICAL();
    return result;
}

/* 覆盖生成syscalls.c中的weak _write；不改第三方printf，也不逐字符阻塞发送。 */
int _write(int file, char *data, int length)
{
    if (file != 1 && file != 2) { errno = EBADF; return -1; }
    if (length < 0 || (data == NULL && length != 0)) { errno = EINVAL; return -1; }
    if (length == 0) return 0;
    size_t consumed = log_console_write(data, (size_t)length);
    if (consumed == 0U) { errno = EAGAIN; return -1; }
    return (int)consumed;
}
