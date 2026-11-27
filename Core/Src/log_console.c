/* USART1与TCP调试控制台共用命令执行，各自保存输入、来源和控制回复。 */
#include "log_console.h"
#include "net_console.h"
#include "handoff_mcu.h"
#include "rk_ddr_capacity.h"
#include "rk_task_control.h"
#include "pwr_ctrl.h"
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
#define LOG_COMMAND_CAPACITY 32U /* 包括NUL，可容纳set CASWriteLatency及最大u32十进制值。 */
static const char s_rk_escape[] = "log mcu";

typedef struct {
    uint32_t session; /* 串口固定1，网络为连接代数；0表示未连接。 */
    uint8_t source, status_pending, discard_line, rk_line_start, skip_lf;
    char command[LOG_COMMAND_CAPACITY];
    size_t command_length, rk_escape_length;
    const char *reply;
    char formatted_reply[96]; /* 每端各自保留，输出队列满时由flush_reply重试。 */
    char debug_command[LOG_COMMAND_CAPACITY];
    char debug_reply[LOG_CONSOLE_RECORD_SIZE];
} console_t;

typedef struct {
    console_t *owner;
    uint32_t session;
    log_console_power_request_t request;
    uint8_t taken;
    char reply[128];
} power_slot_t;

/* 0号是USART1，后续各项对应TCP槽位，命令半行和控制回复均按会话隔离。 */
enum { CONSOLE_COUNT = 1U + NET_CONSOLE_CLIENT_COUNT };
static console_t s_consoles[CONSOLE_COUNT];
static console_t *const s_serial = &s_consoles[0];
/* RK输入只允许一个写入者，所有控制台共用一块重试缓冲。 */
static uint8_t s_rk_pending[UART_RX_BUF_SIZE + sizeof(s_rk_escape) - 1U];
static size_t s_rk_pending_length;
static unsigned s_next_status;
static console_t *s_rk_owner, *s_status_owner;
static uint32_t s_status_session;
static uint8_t s_started;
static uint8_t s_site_enabled = 1U;
static power_slot_t s_power, s_force;
/* 与RK单在途约束一致。算法名在领取后清空，槽位保留至终态已送达或原会话断开。 */
static struct {
    console_t *owner;
    uint32_t session;
    char name[LOG_CONSOLE_FUNC_NAME_SIZE];
    char reply[LOG_CONSOLE_RECORD_SIZE];
} s_func;
static volatile uint32_t s_dropped_bytes;

/* 调试串口常驻；TCP槽位复用后必须按新连接代数校验旧命令。 */
static uint32_t current_session(const console_t *console)
{
    if (console == s_serial) return 1U;
    return net_console_session((unsigned)(console - s_consoles) - 1U);
}

static void count_drop(size_t bytes)
{
    s_dropped_bytes = bytes > UINT32_MAX - s_dropped_bytes ?
                      UINT32_MAX : s_dropped_bytes + (uint32_t)bytes;
}

/* 调用方持有短临界区；仅复制有界数据，不执行网络API或等待串口空间。 */
static int output(console_t *console, const void *data, size_t length, int drop)
{
    if (!console->session) return -1;
    if (console != s_serial)
        return net_console_write(console->session, data, length, drop);
    int sent = uart_send(UART_PORT_CONSOLE, (uint8_t *)data, (uint16_t)length) == IF_OK;
    if (!sent && drop) count_drop(length);
    return sent;
}

static void flush_power_reply(console_t *console, power_slot_t *slot)
{
    if (slot->owner == console && slot->reply[0]) {
        if (console->session == slot->session &&
            output(console, slot->reply, strlen(slot->reply), 0) == 0) return;
        memset(slot, 0, sizeof(*slot));
    }
}

static void flush_reply(console_t *console)
{
    if (console->reply) {
        if (output(console, console->reply, strlen(console->reply), 0) == 0) return;
        console->reply = NULL;
    }
    flush_power_reply(console, &s_power);
    flush_power_reply(console, &s_force);
    if (s_func.owner == console && s_func.reply[0] &&
        (console->session != s_func.session ||
         output(console, s_func.reply, strlen(s_func.reply), 0) != 0))
        memset(&s_func, 0, sizeof(s_func));
    if (console->debug_reply[0] &&
        output(console, console->debug_reply, strlen(console->debug_reply), 0) != 0)
        console->debug_reply[0] = '\0';
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
        if (s_serial->source == source && s_serial->reply) {
            count_drop(chunk);
            serial_sent += chunk;
        } else if (s_serial->source == source && !serial_failed) {
            if (output(s_serial, data + offset, chunk, 1) > 0) serial_sent += chunk;
            else {
                count_drop(length - offset - chunk);
                serial_failed = 1;
            }
        } else if (s_serial->source != source) serial_sent += chunk;
        for (unsigned slot = 0U; slot < NET_CONSOLE_CLIENT_COUNT; ++slot) {
            console_t *console = &s_consoles[slot + 1U];
            if (console->session && console->source == source)
                (void)output(console, data + offset, chunk, 1);
        }
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
    int selected = 0;
    if (s_started) {
        for (unsigned i = 0U; i < CONSOLE_COUNT; ++i) {
            if (s_consoles[i].session && s_consoles[i].source == LOG_SOURCE_MCU) {
                selected = 1;
                break;
            }
        }
    }
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
    memset(s_consoles, 0, sizeof(s_consoles));
    s_rk_pending_length = 0U;
    s_next_status = 0U;
    s_serial->session = 1U;
    s_serial->source = B1_LOG_DEFAULT_SOURCE;
    s_serial->rk_line_start = 1U;
    s_rk_owner = s_serial->source == LOG_SOURCE_RK ? s_serial : NULL;
    s_status_owner = s_serial;
    s_status_session = s_serial->session;
    memset(&s_power, 0, sizeof(s_power));
    memset(&s_force, 0, sizeof(s_force));
    memset(&s_func, 0, sizeof(s_func));
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
        (void)snprintf(console->formatted_reply, sizeof(console->formatted_reply),
                       "\r\n[led] states=%u,%u,%u; 0=off 1=pass 2=fail 3=normal 4=hot 5=both\r\n",
                       (unsigned)states[0], (unsigned)states[1], (unsigned)states[2]);
        return console->formatted_reply;
    }
    return usage;
}

static const char *power_name(log_console_power_request_t request)
{
    if (request == LOG_CONSOLE_POWER_START_RK) return "start rk";
    if (request == LOG_CONSOLE_POWER_START_LPDDR4) return "start rk LPDDR4";
    if (request == LOG_CONSOLE_POWER_FORCE_OFF) return "end rk force";
    return "end rk";
}

/* 调用方持临界区；完整终态留在原请求槽内，直到该端收下或断开。 */
static void complete_power_locked(power_slot_t *slot, const char *error)
{
    if (!slot->taken || slot->reply[0]) return;
    if (error) (void)snprintf(slot->reply, sizeof(slot->reply), "\r\n[rk power] %s failed: %.40s\r\n",
                             power_name(slot->request), error);
    else if (slot->request == LOG_CONSOLE_POWER_START_RK)
        strcpy(slot->reply, "\r\n[rk power] power ON; LPDDR4X 1800/1100/600mV; fspiM1; check RK boot log\r\n");
    else if (slot->request == LOG_CONSOLE_POWER_START_LPDDR4)
        strcpy(slot->reply, "\r\n[rk power] power ON; LPDDR4 1800/1100/1100mV; fspiM1; check RK boot log\r\n");
    else strcpy(slot->reply, "\r\n[rk power] power OFF\r\n");
}

static void cancel_unclaimed_power(void)
{
    if (s_power.request != LOG_CONSOLE_POWER_NONE && !s_power.taken) {
        s_power.taken = 1U;
        complete_power_locked(&s_power, "cancelled");
    }
}

static void cancel_unclaimed_func(void)
{
    if (s_func.name[0]) {
        s_func.name[0] = '\0';
        strcpy(s_func.reply, "\r\n[func] cancelled\r\n");
    }
}

static void queue_power(console_t *console, log_console_power_request_t request)
{
    power_slot_t *slot = request == LOG_CONSOLE_POWER_FORCE_OFF ? &s_force : &s_power;
    if (s_force.request || slot->request) {
        console->reply = "\r\n[rk power] busy\r\n";
        return;
    }
    if (slot == &s_force) {
        cancel_unclaimed_power();
        cancel_unclaimed_func();
    }
    slot->request = request;
    slot->owner = console;
    slot->session = console->session;
    (void)snprintf(console->formatted_reply, sizeof(console->formatted_reply),
                   "\r\n[rk power] %s queued\r\n", power_name(request));
    console->reply = console->formatted_reply;
}

static void queue_debug(console_t *console)
{
    if (console->debug_command[0] || console->debug_reply[0]) {
        console->reply = "\r\n[debug] busy\r\n";
        return;
    }
    memcpy(console->debug_command, console->command, console->command_length + 1U);
}

static void queue_func(console_t *console, const char *name)
{
    if (!name[0] || strchr(name, ' ')) {
        console->reply = "\r\n[func] use: func <algorithmName>\r\n";
    } else if (s_func.owner || s_force.request) {
        console->reply = "\r\n[func] busy\r\n";
    } else {
        _Static_assert(LOG_COMMAND_CAPACITY <= LOG_CONSOLE_FUNC_NAME_SIZE + 5U, "FUNC算法名须容纳命令行参数");
        s_func.owner = console;
        s_func.session = console->session;
        strcpy(s_func.name, name);
        (void)snprintf(console->formatted_reply, sizeof(console->formatted_reply),
                       "\r\n[func] %s queued\r\n", name);
        console->reply = console->formatted_reply;
    }
}

static void finish_command(console_t *console)
{
    if (console->command_length == 0U && !console->discard_line) return; /* 忽略空行及CRLF的LF。 */
    console->command[console->command_length] = '\0';
    taskENTER_CRITICAL();
    if (console->session != current_session(console)) {
        console->command_length = 0U;
        taskEXIT_CRITICAL();
        return;
    }
    if (!console->discard_line && strcmp(console->command, "log mcu") == 0) {
        console->source = LOG_SOURCE_MCU;
        if (s_rk_owner == console && !s_rk_pending_length) s_rk_owner = NULL;
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
    } else if (!console->discard_line && strcmp(console->command, "test status") == 0) {
        queue_debug(console);
    } else if (!s_site_enabled) {
        console->reply = "\r\n[log] siteDisabled\r\n";
    } else if (!console->discard_line && (strncmp(console->command, "func ", 5U) == 0 ||
                                         strcmp(console->command, "func") == 0)) {
        queue_func(console, console->command_length > 4U ? console->command + 5U : "");
    } else if (!console->discard_line && strncmp(console->command, "set ", 4U) == 0) {
        queue_debug(console);
    } else if (!console->discard_line && strncmp(console->command, "check ", sizeof("check ") - 1U) == 0) {
        const char *name = console->command + sizeof("check ") - 1U;
        const char *error = rk_task_check_timing(name);
        (void)snprintf(console->formatted_reply, sizeof(console->formatted_reply),
                       "\r\n[RK TIMING] check %s %s\r\n", name, error ? error : "queued");
        console->reply = console->formatted_reply;
    } else if (!console->discard_line && (strcmp(console->command, "start rk") == 0 ||
                                         strcmp(console->command, "start rk LPDDR4X") == 0)) {
        queue_power(console, LOG_CONSOLE_POWER_START_RK);
    } else if (!console->discard_line && strcmp(console->command, "start rk LPDDR4") == 0) {
        queue_power(console, LOG_CONSOLE_POWER_START_LPDDR4);
    } else if (!console->discard_line && strncmp(console->command, "start rk ", 9U) == 0) {
        console->reply = "\r\n[rk power] use: start rk [LPDDR4|LPDDR4X]\r\n";
    } else if (!console->discard_line && strcmp(console->command, "end rk") == 0) {
        queue_power(console, LOG_CONSOLE_POWER_END_RK);
    } else if (!console->discard_line && strcmp(console->command, "end rk force") == 0) {
        queue_power(console, LOG_CONSOLE_POWER_FORCE_OFF);
    } else if (!console->discard_line &&
               (strcmp(console->command, "net rk") == 0 || strcmp(console->command, "net mcu") == 0)) {
        handoff_command_t request = strcmp(console->command, "net rk") == 0 ? HANDOFF_COMMAND_RK : HANDOFF_COMMAND_MCU;
        console->reply = handoff_mcu_console_request(request) ?
            "\r\n[handoff] command queued\r\n" : "\r\n[handoff] command busy\r\n";
    } else if (!console->discard_line && strncmp(console->command, "led ", 4U) == 0) {
        console->reply = apply_led_command(console, console->command);
    } else {
        console->reply = "\r\n[log] use: log mcu | log rk | status | test status | func <algorithmName> | check <timing|all> | set <timing|freq> <value> | start rk [LPDDR4|LPDDR4X] | end rk [force] | net rk | net mcu | led 0|1|2|all 00|01|10|11|off|pass|fail|normal|hot\r\n";
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
    if (console != s_rk_owner) return 1;
    if (console->session != current_session(console)) {
        s_rk_pending_length = 0U;
        s_rk_owner = NULL;
        return 0;
    }
    if (s_rk_pending_length != 0U &&
        uart_send(UART_PORT_RK_DEBUG, s_rk_pending,
                  (uint16_t)s_rk_pending_length) != IF_OK) return 0;
    s_rk_pending_length = 0U;
    if (console->source != LOG_SOURCE_RK) s_rk_owner = NULL;
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
                memcpy(s_rk_pending + s_rk_pending_length, s_rk_escape, console->rk_escape_length);
                s_rk_pending_length += console->rk_escape_length;
            }
            console->rk_escape_length = 0U;
        }
    }
    if (s_rk_owner == console) s_rk_pending[s_rk_pending_length++] = byte;
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
    int length = console == s_serial ? uart_poll_rx(UART_PORT_CONSOLE, buffer, UART_RX_BUF_SIZE) :
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
            if (console->session != current_session(console)) break;
            consume_console_byte(console, buffer[index]);
        }
        (void)flush_rk_input(console);
    }
}

void log_console_process(void)
{
    uint8_t buffer[UART_RX_BUF_SIZE];
    if (!s_started) return;
    taskENTER_CRITICAL();
    for (unsigned slot = 0U; slot < NET_CONSOLE_CLIENT_COUNT; ++slot) {
        console_t *console = &s_consoles[slot + 1U];
        uint32_t session = net_console_session(slot);
        if (session == console->session) continue;
        if (s_rk_owner == console) {
            s_rk_owner = NULL;
            s_rk_pending_length = 0U;
        }
        memset(console, 0, sizeof(*console));
        console->session = session;
        console->rk_line_start = 1U;
        if (session) console->reply = "\r\n[net console] connected; source=mcu; type status or log rk\r\n";
    }
    taskEXIT_CRITICAL();
    for (unsigned i = 0U; i < CONSOLE_COUNT; ++i) {
        console_t *console = &s_consoles[i];
        taskENTER_CRITICAL();
        flush_reply(console);
        taskEXIT_CRITICAL();
        process_input(console, buffer);
        taskENTER_CRITICAL();
        flush_reply(console);
        taskEXIT_CRITICAL();
    }
    /* 即使所有出口都在看MCU，也继续消费一次RK日志，保持容量识别及DMA排空。 */
    int length = uart_poll_rx(UART_PORT_RK_DEBUG, buffer, sizeof(buffer));
    pwr_ctrl_state_t power = pwr_ctrl_get_state();
    bool powered = power == PWR_CTRL_STATE_WAIT_FIRST_RAIL ||
                   power == PWR_CTRL_STATE_WAIT_SECOND_RAIL || power == PWR_CTRL_STATE_ON;
    rk_ddr_capacity_observe(powered, buffer, length);
    if (length > 0) (void)write_source(LOG_SOURCE_RK, buffer, (size_t)length, LOG_TX_CHUNK_SIZE);
}

int log_console_take_status_request(void)
{
    taskENTER_CRITICAL();
    int requested = 0;
    for (unsigned i = 0U; s_started && i < CONSOLE_COUNT; ++i) {
        unsigned index = (s_next_status + i) % CONSOLE_COUNT;
        console_t *console = &s_consoles[index];
        if (!console->status_pending || console->reply || console->source != LOG_SOURCE_MCU ||
            !console->session || console->session != current_session(console)) continue;
        console->status_pending = 0U;
        s_status_owner = console;
        s_status_session = console->session;
        s_next_status = (index + 1U) % CONSOLE_COUNT;
        requested = 1;
        break;
    }
    taskEXIT_CRITICAL();
    return requested;
}

void log_console_service_debug(log_console_debug_handler_t handler)
{
    if (!handler || !s_started) return;
    for (unsigned i = 0U; i < CONSOLE_COUNT; ++i) {
        console_t *console = &s_consoles[i];
        char command[LOG_COMMAND_CAPACITY];
        taskENTER_CRITICAL();
        uint32_t session = console->session;
        int pending = session && console->debug_command[0] && session == current_session(console);
        if (pending) memcpy(command, console->debug_command, sizeof(command));
        taskEXIT_CRITICAL();
        if (!pending) continue;

        char reply[LOG_CONSOLE_RECORD_SIZE];
        handler(command, reply, sizeof(reply));
        taskENTER_CRITICAL();
        if (console->session == session && current_session(console) == session) {
            strcpy(console->debug_reply, reply);
            console->debug_command[0] = '\0';
        }
        taskEXIT_CRITICAL();
    }
}

log_console_power_request_t log_console_take_power_request(void)
{
    taskENTER_CRITICAL();
    log_console_power_request_t request = LOG_CONSOLE_POWER_NONE;
    if (s_power.request && !s_power.taken &&
        (!s_power.owner->reply || s_power.owner->session != s_power.session)) {
        request = s_power.request;
        s_power.taken = 1U;
    }
    taskEXIT_CRITICAL();
    return request;
}

int log_console_take_force_off(void)
{
    taskENTER_CRITICAL();
    int requested = s_force.request && !s_force.taken;
    if (requested) s_force.taken = 1U;
    taskEXIT_CRITICAL();
    return requested;
}

void log_console_complete_power_request(const char *error)
{
    taskENTER_CRITICAL();
    complete_power_locked(&s_power, error);
    taskEXIT_CRITICAL();
}

void log_console_complete_force_off(const char *error)
{
    taskENTER_CRITICAL();
    complete_power_locked(&s_force, error);
    taskEXIT_CRITICAL();
}

int log_console_take_func(char name[LOG_CONSOLE_FUNC_NAME_SIZE])
{
    int taken = 0;
    taskENTER_CRITICAL();
    if (name && s_func.name[0]) {
        if (current_session(s_func.owner) != s_func.session) {
            memset(&s_func, 0, sizeof(s_func));
        } else {
            strcpy(name, s_func.name);
            s_func.name[0] = '\0';
            taken = 1;
        }
    }
    taskEXIT_CRITICAL();
    return taken;
}

void log_console_complete_func(const char *text)
{
    taskENTER_CRITICAL();
    if (text && s_func.owner && !s_func.name[0] && !s_func.reply[0])
        (void)snprintf(s_func.reply, sizeof(s_func.reply), "%s", text);
    taskEXIT_CRITICAL();
}

void log_console_set_site_enabled(uint8_t enabled)
{
    taskENTER_CRITICAL();
    s_site_enabled = enabled;
    /* 命令可能在关站前已入槽，但“queued”仍堵在输出端；不能留到再开站才启动RK。 */
    if (!enabled) {
        cancel_unclaimed_power();
        cancel_unclaimed_func();
        for (unsigned i = 0U; i < CONSOLE_COUNT; ++i) {
            console_t *console = &s_consoles[i];
            if (strncmp(console->debug_command, "set ", 4U) == 0) {
                console->debug_command[0] = '\0';
                strcpy(console->debug_reply, "\r\n[debug] siteDisabled\r\n");
            }
        }
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
