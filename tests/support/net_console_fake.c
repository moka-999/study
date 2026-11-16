/* 串口控制台测试的网络边界；真正TCP资源和连接回归另链接实际lwIP。 */
#include "net_console.h"
#include <string.h>

uint32_t fake_net_session;
int fake_net_blocked;
char fake_net_output[16384];
size_t fake_net_output_length;
uint8_t fake_net_input[512];
size_t fake_net_input_length;

uint32_t net_console_session(void) { return fake_net_session; }
size_t net_console_read(uint32_t session, uint8_t *data, size_t capacity)
{
    if (!session || session != fake_net_session) return 0;
    size_t length = fake_net_input_length < capacity ? fake_net_input_length : capacity;
    memcpy(data, fake_net_input, length);
    memmove(fake_net_input, fake_net_input + length, fake_net_input_length - length);
    fake_net_input_length -= length;
    return length;
}
int net_console_write(uint32_t session, const void *data, size_t length, int drop)
{
    (void)drop;
    if (!session || session != fake_net_session) return -1;
    if (fake_net_blocked || length >= sizeof(fake_net_output) - fake_net_output_length) return 0;
    memcpy(fake_net_output + fake_net_output_length, data, length);
    fake_net_output_length += length;
    fake_net_output[fake_net_output_length] = '\0';
    return 1;
}
uint32_t net_console_dropped_bytes(void) { return 0; }
