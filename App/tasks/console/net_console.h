#ifndef B1_NET_CONSOLE_H
#define B1_NET_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

#define NET_CONSOLE_CLIENT_COUNT 2U

/* 默认任务在lwIP初始化后启用，并周期调用process；不创建任务或修改业务连接。 */
void net_console_start(void);
void net_console_process(void);

/* 返回指定连接槽的会话号；槽号越界或空闲时返回0，重连后旧会话号失效。 */
uint32_t net_console_session(unsigned slot);
size_t net_console_read(uint32_t session, uint8_t *data, size_t capacity);

/* 每条1..256字节，整条缓存：1成功、0拥塞、-1断线/会话过期。
 * 调用方可按drop统计普通日志丢失；控制回复传0，稍后重试。 */
int net_console_write(uint32_t session, const void *data, size_t length, int drop);
/* 丢弃字节数按连接槽跨重连累计，总数或单槽计数均饱和于UINT32_MAX。 */
uint32_t net_console_dropped_bytes(void);
uint32_t net_console_client_dropped_bytes(unsigned slot);

#endif
