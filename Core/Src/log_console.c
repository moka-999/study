/* 本地命令与日志复用USART1；输出排队失败只损失日志，不等待串口排空。 */
#include "log_console.h"
#include "uart_router.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>

enum { LOG_SOURCE_MCU = 0, LOG_SOURCE_RK = 1 };
#define LOG_TX_CHUNK_SIZE 128U
#define LOG_COMMAND_CAPACITY 16U

static uint8_t s_source = B1_LOG_DEFAULT_SOURCE;
static uint8_t s_started;
static char s_command[LOG_COMMAND_CAPACITY];
static size_t s_command_length;
static uint8_t s_discard_line;
static const char *s_reply;
/* 调试器可读取本次开机的TX拥塞丢字节数；主动隐藏另一来源不计为错误。 */
static volatile uint32_t s_dropped_bytes;

/* 调用方已进入短临界区；USART1发送取锁必须为0等待，避免屏蔽中断时睡眠。 */
static void flush_reply(void)
{
    if (s_reply != NULL &&
        uart_send(UART_PORT_CONSOLE, (uint8_t *)s_reply,
                  (uint16_t)strlen(s_reply)) == IF_OK) {
        s_reply = NULL;
    }
}

static size_t write_source(uint8_t source, const uint8_t *data, size_t length)
{
    if (data == NULL || __get_IPSR() != 0U) return 0U;
    size_t offset = 0U;
    while (offset < length) {
        uint16_t chunk = (uint16_t)((length - offset > LOG_TX_CHUNK_SIZE) ?
                                   LOG_TX_CHUNK_SIZE : length - offset);
        /* 将来源检查与排队作为一个原子操作，防止切换确认后混入旧来源。
         * 每次最多复制128字节，不在临界区等待发送完成。 */
        taskENTER_CRITICAL();
        if (!s_started) {
            taskEXIT_CRITICAL();
            return 0U;
        }
        if (s_source != source || s_reply != NULL) {
            taskEXIT_CRITICAL();
            return length; /* 未选中或等待控制回复时，日志不缓存、不补发。 */
        }
        int status = uart_send(UART_PORT_CONSOLE, (uint8_t *)data + offset, chunk);
        if (status != IF_OK) {
            size_t lost = length - offset;
            s_dropped_bytes = lost > UINT32_MAX - s_dropped_bytes ?
                              UINT32_MAX : s_dropped_bytes + (uint32_t)lost;
        }
        taskEXIT_CRITICAL();
        if (status != IF_OK) break;
        offset += chunk;
    }
    return offset;
}

size_t log_console_write(const char *text, size_t length)
{
    return write_source(LOG_SOURCE_MCU, (const uint8_t *)text, length);
}

void log_console_start(void)
{
    static const char banner[] = "B1 gateway software ready; power remains OFF\r\n";
    taskENTER_CRITICAL();
    s_source = B1_LOG_DEFAULT_SOURCE;
    s_command_length = 0U;
    s_discard_line = 0U;
    s_reply = NULL;
    s_started = 1U;
    taskEXIT_CRITICAL();
    (void)log_console_write(banner, sizeof(banner) - 1U);
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
        s_reply = "\r\n[log] source=rk\r\n";
    } else {
        s_reply = "\r\n[log] use: log mcu | log rk\r\n";
    }
    /* 确认串排在既有TX尾部；队列满则下一轮重试，期间不加入新日志。
     * 命令洪泛时只保留最新控制回复，实际选择以最后一条有效命令为准。 */
    flush_reply();
    taskEXIT_CRITICAL();
    s_command_length = 0U;
    s_discard_line = 0U;
}

void log_console_process(void)
{
    uint8_t buffer[UART_RX_BUF_SIZE];
    if (!s_started) return;
    int length = uart_poll_rx(UART_PORT_CONSOLE, buffer, sizeof(buffer));
    if (length == UART_RX_GAP) {
        s_command_length = 0U;
        s_discard_line = 1U; /* 断流后丢弃到换行，不能把残余后缀当成新命令。 */
    } else {
        for (int index = 0; index < length; ++index) {
            uint8_t byte = buffer[index];
            if (byte == '\r' || byte == '\n') {
                finish_command();
            } else if (!s_discard_line) {
                if (byte < 0x20U || byte > 0x7EU ||
                    s_command_length >= sizeof(s_command) - 1U) {
                    s_discard_line = 1U;
                } else {
                    s_command[s_command_length++] = (char)byte;
                }
            }
        }
    }
    taskENTER_CRITICAL();
    flush_reply();
    taskEXIT_CRITICAL();

    /* MCU模式仍持续消费RK日志，避免切换时回放积压或堵住接收恢复。 */
    length = uart_poll_rx(UART_PORT_RK_DEBUG, buffer, sizeof(buffer));
    if (length > 0) (void)write_source(LOG_SOURCE_RK, buffer, (size_t)length);
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
