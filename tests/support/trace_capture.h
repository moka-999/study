#ifndef TEST_TRACE_CAPTURE_H
#define TEST_TRACE_CAPTURE_H

/* 仅供QEMU：在指定用例期间采集实际MCU记录，stop后恢复静默。 */
void trace_capture_begin(void);
void trace_capture_stop(void);
const char *trace_capture_text(void);
void trace_capture_append(const char *format, ...);

#endif
