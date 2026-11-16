#ifndef SERIAL_NET_TEST_LOG_CONSOLE_H
#define SERIAL_NET_TEST_LOG_CONSOLE_H
#include <stddef.h>
/* V2未提供B1日志口；仅声明未被本过滤测试调用的接口。 */
size_t log_console_write(const char *text, size_t length);
#endif
