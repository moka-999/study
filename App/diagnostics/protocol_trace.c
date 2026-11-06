/* B1协议诊断的共用出口。格式化在任务上下文执行，短临界区只保护计数快照。 */
#include "protocol_trace.h"
#include "log_console.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t rx, tx, errors, last_ms;
} trace_counts_t;
static trace_counts_t s_counts[TRACE_GROUP_COUNT];

const char *trace_u64(uint64_t value, char buffer[TRACE_U64_TEXT_SIZE])
{
    /* 精简版printf不保证支持%llu；从末位写入，0也保留一位。 */
    char *p = buffer + TRACE_U64_TEXT_SIZE - 1U;
    *p = '\0';
    do {
        *--p = (char)('0' + value % 10U);
        value /= 10U;
    } while (value);
    return p;
}

void trace_line(const char *tag, const char *format, ...)
{
    if (!log_console_mcu_selected()) return;
    char line[LOG_CONSOLE_RECORD_SIZE];
    int head = snprintf(line, sizeof(line), "[%lu ms][%s] ", (unsigned long)HAL_GetTick(), tag);
    if (head < 0 || (size_t)head >= sizeof(line) - 2U) return;
    va_list args;
    va_start(args, format);
    size_t space = sizeof(line) - (size_t)head - 2U;
    int body = vsnprintf(line + head, space, format, args);
    va_end(args);
    if (body < 0) return;
    size_t used = (size_t)head + (size_t)body;
    if ((size_t)body >= space) {
        used = sizeof(line) - 3U;
        memcpy(line + used - 3U, "...", 3U);
    }
    memcpy(line + used, "\r\n", 2U);
    log_console_write_record(line, used + 2U);
}

void trace_count(trace_group_t group, trace_direction_t direction)
{
    uint32_t now = HAL_GetTick();
    taskENTER_CRITICAL();
    trace_counts_t *s = &s_counts[group];
    uint32_t *value = direction == TRACE_RX ? &s->rx : direction == TRACE_TX ? &s->tx : &s->errors;
    if (*value != UINT32_MAX) ++*value;
    s->last_ms = now;
    taskEXIT_CRITICAL();
}

void trace_report(void (*append)(const char *, ...))
{
    static const char *const names[TRACE_GROUP_COUNT] = {
        [TRACE_PC_HEARTBEAT] = "PC heartbeat", [TRACE_PC_SAMPLE] = "PC sample",
        [TRACE_RK_HEARTBEAT] = "RK heartbeat"
    };
    trace_counts_t counts[TRACE_GROUP_COUNT];
    taskENTER_CRITICAL();
    memcpy(counts, s_counts, sizeof(counts));
    taskEXIT_CRITICAL();
    for (unsigned i = 0; i < TRACE_GROUP_COUNT; ++i)
        append("[traffic] %s rx=%lu tx=%lu errors=%lu lastMs=%lu\r\n", names[i],
               (unsigned long)counts[i].rx, (unsigned long)counts[i].tx,
               (unsigned long)counts[i].errors, (unsigned long)counts[i].last_ms);
    append("[console] droppedBytes=%lu\r\n", (unsigned long)log_console_dropped_bytes());
}
