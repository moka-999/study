#include "trace_capture.h"
#include "log_console.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char s_text[4096];
static size_t s_used;
static int s_enabled;

void trace_capture_begin(void)
{
    s_used = 0U;
    s_text[0] = '\0';
    s_enabled = 1;
}
void trace_capture_stop(void) { s_enabled = 0; }
const char *trace_capture_text(void) { return s_text; }
int log_console_mcu_selected(void) { return s_enabled; }
uint32_t log_console_dropped_bytes(void) { return 0U; }

void log_console_write_record(const char *text, size_t length)
{
    if (s_enabled && length < sizeof(s_text) - s_used) {
        memcpy(s_text + s_used, text, length);
        s_used += length;
        s_text[s_used] = '\0';
    }
}

void trace_capture_append(const char *format, ...)
{
    char line[LOG_CONSOLE_RECORD_SIZE];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length > 0 && (size_t)length < sizeof(line)) log_console_write_record(line, (size_t)length);
}
