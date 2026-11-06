/* 本地命令与日志复用USART1；输出排队失败只损失日志，不等待串口排空。 */
#include "log_console.h"
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

static uint8_t s_source = B1_LOG_DEFAULT_SOURCE;
static uint8_t s_started;
static uint8_t s_status_pending;
static char s_command[LOG_COMMAND_CAPACITY];
static size_t s_command_length;
static uint8_t s_discard_line;
static const char *s_reply;
static char s_led_reply[96];
static log_console_power_request_t s_power_request;
static uint8_t s_power_taken, s_power_reply_ready;
static char s_power_reply[128];
/* 仅行首保留退出命令前缀；普通字符立即进入RK发送缓存，不限制命令行长度。 */
static const char s_rk_escape[] = "log mcu";
static size_t s_rk_escape_length;
static uint8_t s_rk_line_start = 1U;
static uint8_t s_skip_lf;
/* 一块输入最多加上上一块暂存的7字节前缀；未送完时不继续取控制台输入。 */
static uint8_t s_rk_pending[UART_RX_BUF_SIZE + sizeof(s_rk_escape) - 1U];
static size_t s_rk_pending_length;
/* 已选来源因出口拥塞或控制回复优先而丢掉的字节；主动隐藏另一来源不计。 */
static volatile uint32_t s_dropped_bytes;

/* 调用方持有短临界区；多任务共享累计值，达到上限后保持。 */
static void count_drop(size_t bytes)
{
    s_dropped_bytes = bytes > UINT32_MAX - s_dropped_bytes ?
                      UINT32_MAX : s_dropped_bytes + (uint32_t)bytes;
}

/* 调用方已进入短临界区；USART1发送取锁必须为0等待，避免屏蔽中断时睡眠。 */
static void flush_reply(void)
{
    if (s_reply == NULL && s_power_reply_ready) s_reply = s_power_reply;
    if (s_reply != NULL &&
        uart_send(UART_PORT_CONSOLE, (uint8_t *)s_reply,
                  (uint16_t)strlen(s_reply)) == IF_OK) {
        if (s_reply == s_power_reply) {
            s_power_reply_ready = 0U;
            s_power_request = LOG_CONSOLE_POWER_NONE;
        }
        s_reply = NULL;
    }
}

static size_t write_source(uint8_t source, const uint8_t *data, size_t length, size_t chunk_limit)
{
    if (data == NULL || __get_IPSR() != 0U) return 0U;
    size_t offset = 0U;
    while (offset < length) {
        uint16_t chunk = (uint16_t)((length - offset > chunk_limit) ? chunk_limit : length - offset);
        /* 将来源检查与排队作为一个原子操作，防止切换确认后混入旧来源。
         * 普通流每次最多复制128字节，协议记录单次最多256字节，不等待TX排空。 */
        taskENTER_CRITICAL();
        if (!s_started) {
            taskEXIT_CRITICAL();
            return 0U;
        }
        if (s_source != source || s_reply != NULL) {
            if (s_source == source) count_drop(length - offset);
            taskEXIT_CRITICAL();
            return length; /* 未选中或等待控制回复时，日志不缓存、不补发。 */
        }
        int status = uart_send(UART_PORT_CONSOLE, (uint8_t *)data + offset, chunk);
        if (status != IF_OK) count_drop(length - offset);
        taskEXIT_CRITICAL();
        if (status != IF_OK) break;
        offset += chunk;
    }
    return offset;
}

size_t log_console_write(const char *text, size_t length)
{
    return write_source(LOG_SOURCE_MCU, (const uint8_t *)text, length, LOG_TX_CHUNK_SIZE);
}

int log_console_mcu_selected(void)
{
    taskENTER_CRITICAL();
    int selected = s_started && s_source == LOG_SOURCE_MCU;
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
    s_source = B1_LOG_DEFAULT_SOURCE;
    s_status_pending = 0U;
    s_command_length = 0U;
    s_discard_line = 0U;
    s_reply = NULL;
    s_power_request = LOG_CONSOLE_POWER_NONE;
    s_power_taken = s_power_reply_ready = 0U;
    s_rk_escape_length = 0U;
    s_rk_line_start = 1U;
    s_skip_lf = 0U;
    s_rk_pending_length = 0U;
    s_started = 1U;
    taskEXIT_CRITICAL();
    (void)log_console_write(banner, sizeof(banner) - 1U);
}

/* 仅MCU控制台用于空闲验灯；指定一组时其他组灭，不控制加热或改变PC轮次。
 * 使用正式驱动的状态与500ms相位；后续PC/B2显示事件仍可覆盖本次设置。 */
static const char *apply_led_command(const char *command)
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
        (void)snprintf(s_led_reply, sizeof(s_led_reply),
                       "\r\n[led] states=%u,%u,%u; 0=off 1=pass 2=fail 3=normal 4=hot 5=both\r\n",
                       (unsigned)states[0], (unsigned)states[1], (unsigned)states[2]);
        return s_led_reply;
    }
    return usage;
}

static void finish_command(void)
{
    if (s_command_length == 0U && !s_discard_line) return; /* 忽略空行及CRLF的LF。 */
    s_command[s_command_length] = '\0';
    taskENTER_CRITICAL();
    if (!s_discard_line && strcmp(s_command, "log mcu") == 0) {
        s_source = LOG_SOURCE_MCU;
        s_reply = "\r\n[log] source=mcu\r\n";
    } else if (!s_discard_line && strcmp(s_command, "log rk") == 0) {
        s_source = LOG_SOURCE_RK;
        s_rk_escape_length = 0U;
        s_rk_line_start = 1U;
        s_status_pending = 0U;
        s_reply = "\r\n[log] source=rk\r\n";
    } else if (!s_discard_line && strcmp(s_command, "status") == 0) {
        if (s_source == LOG_SOURCE_MCU) {
            s_status_pending = 1U; /* 合并重复查询；状态由默认任务另行读取。 */
            s_reply = "\r\n[log] status queued\r\n";
        } else {
            s_reply = "\r\n[log] select MCU first: log mcu\r\n";
        }
    } else if (!s_discard_line &&
               (strcmp(s_command, "start rk") == 0 || strcmp(s_command, "end rk") == 0)) {
        /* 只入单槽请求，电源编排由唯一TCP上下文处理；此处仍在短临界区内。 */
        if (s_power_request != LOG_CONSOLE_POWER_NONE) s_reply = "\r\n[rk power] busy\r\n";
        else {
            s_power_request = s_command[0] == 's' ? LOG_CONSOLE_POWER_START_RK : LOG_CONSOLE_POWER_END_RK;
            s_power_taken = 0U;
            s_reply = s_power_request == LOG_CONSOLE_POWER_START_RK ?
                "\r\n[rk power] start rk queued\r\n" : "\r\n[rk power] end rk queued\r\n";
        }
    } else if (!s_discard_line && strncmp(s_command, "led ", 4U) == 0) {
        s_reply = apply_led_command(s_command);
    } else {
        s_reply = "\r\n[log] use: log mcu | log rk | status | start rk | end rk | led 0|1|2|all 00|01|10|11|off|pass|fail|normal|hot\r\n";
    }
    /* 确认串排在既有TX尾部；队列满则下一轮重试，期间不加入新日志。
     * 命令洪泛时只保留最新控制回复，实际选择以最后一条有效命令为准。 */
    flush_reply();
    taskEXIT_CRITICAL();
    s_command_length = 0U;
    s_discard_line = 0U;
}

/* 发送环整块接纳；满时保留原数据，下次推进不重复已接纳的字符。 */
static int flush_rk_input(void)
{
    if (s_rk_pending_length != 0U &&
        uart_send(UART_PORT_RK_DEBUG, s_rk_pending,
                  (uint16_t)s_rk_pending_length) != IF_OK) return 0;
    s_rk_pending_length = 0U;
    return 1;
}

static void forward_rk_byte(uint8_t byte)
{
    if (s_rk_line_start) {
        if (byte == 0x03U) {
            /* Ctrl+C取消尚未发出的退出前缀，并立即交给RK中断当前输入。 */
            s_rk_escape_length = 0U;
        } else if ((byte == 0x08U || byte == 0x7FU) && s_rk_escape_length != 0U) {
            --s_rk_escape_length;
            return;
        } else if ((byte == '\r' || byte == '\n') &&
                   s_rk_escape_length == sizeof(s_rk_escape) - 1U) {
            memcpy(s_command, s_rk_escape, sizeof(s_rk_escape));
            s_command_length = sizeof(s_rk_escape) - 1U;
            s_rk_escape_length = 0U;
            finish_command();
            s_skip_lf = byte == '\r';
            return;
        } else if (s_rk_escape_length < sizeof(s_rk_escape) - 1U &&
                   byte == (uint8_t)s_rk_escape[s_rk_escape_length]) {
            ++s_rk_escape_length;
            return;
        } else {
            memcpy(s_rk_pending + s_rk_pending_length, s_rk_escape, s_rk_escape_length);
            s_rk_pending_length += s_rk_escape_length;
            s_rk_escape_length = 0U;
        }
    }
    s_rk_pending[s_rk_pending_length++] = byte;
    s_rk_line_start = byte == '\r' || byte == '\n' || byte == 0x03U;
}

static void consume_console_byte(uint8_t byte)
{
    if (s_skip_lf) {
        s_skip_lf = 0U;
        if (byte == '\n') return; /* 本地切换行的CRLF不能把LF泄漏给RK。 */
    }
    if (s_discard_line) {
        if (byte == '\r' || byte == '\n') {
            if (s_source == LOG_SOURCE_MCU) finish_command();
            s_discard_line = 0U;
            s_rk_line_start = 1U;
            s_skip_lf = byte == '\r';
        }
        return;
    }
    if (s_source == LOG_SOURCE_RK) {
        forward_rk_byte(byte);
    } else if (byte == '\r' || byte == '\n') {
        finish_command();
        s_skip_lf = byte == '\r';
    } else if (byte < 0x20U || byte > 0x7EU ||
               s_command_length >= sizeof(s_command) - 1U) {
        s_discard_line = 1U;
    } else {
        s_command[s_command_length++] = (char)byte;
    }
}

void log_console_process(void)
{
    uint8_t buffer[UART_RX_BUF_SIZE];
    if (!s_started) return;
    int length = 0;
    if (flush_rk_input()) {
        length = uart_poll_rx(UART_PORT_CONSOLE, buffer, sizeof(buffer));
        if (length == UART_RX_GAP) {
            s_command_length = 0U;
            s_rk_escape_length = 0U;
            s_skip_lf = 0U;
            s_discard_line = 1U; /* 丢弃至换行，不把残缺输入作为新命令转发。 */
            taskENTER_CRITICAL();
            s_reply = "\r\n[log] input lost; discard until newline\r\n";
            taskEXIT_CRITICAL();
        } else {
            for (int index = 0; index < length; ++index) consume_console_byte(buffer[index]);
            (void)flush_rk_input();
        }
    }
    taskENTER_CRITICAL();
    flush_reply();
    taskEXIT_CRITICAL();

    /* MCU模式仍持续消费RK日志，避免切换时回放积压或堵住接收恢复。 */
    length = uart_poll_rx(UART_PORT_RK_DEBUG, buffer, sizeof(buffer));
    if (length > 0) (void)write_source(LOG_SOURCE_RK, buffer, (size_t)length, LOG_TX_CHUNK_SIZE);
}

int log_console_take_status_request(void)
{
    taskENTER_CRITICAL();
    int requested = s_started && s_source == LOG_SOURCE_MCU &&
                    s_status_pending && s_reply == NULL;
    if (requested) s_status_pending = 0U;
    taskEXIT_CRITICAL();
    return requested;
}

log_console_power_request_t log_console_take_power_request(void)
{
    taskENTER_CRITICAL();
    log_console_power_request_t request = LOG_CONSOLE_POWER_NONE;
    if (s_started && s_power_request != LOG_CONSOLE_POWER_NONE && !s_power_taken && s_reply == NULL) {
        request = s_power_request;
        s_power_taken = 1U;
    }
    taskEXIT_CRITICAL();
    return request;
}

void log_console_complete_power_request(const char *error)
{
    taskENTER_CRITICAL();
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
    taskEXIT_CRITICAL();
}

int log_console_write_status(const char *text, size_t length)
{
    if (text == NULL || length == 0U || length > LOG_TX_CHUNK_SIZE ||
        __get_IPSR() != 0U) return -1;
    taskENTER_CRITICAL();
    int result;
    if (!s_started || s_source != LOG_SOURCE_MCU) result = -1;
    else if (s_reply != NULL) result = 0;
    else result = uart_send(UART_PORT_CONSOLE, (uint8_t *)text, (uint16_t)length) == IF_OK;
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
