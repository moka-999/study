#ifndef B1_NET_CONSOLE_H
#define B1_NET_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

/* 默认任务在lwIP初始化后启用，并周期调用process；不创建任务或修改业务连接。 */
void net_console_start(void);
void net_console_process(void);

/* 非零值标识当前连接；重连换代，旧命令/回复不得进入新连接。 */
uint32_t net_console_session(void);
size_t net_console_read(uint32_t session, uint8_t *data, size_t capacity);

/* 每条1..256字节，整条缓存：1成功、0拥塞、-1断线/会话过期。
 * 调用方可按drop统计普通日志丢失；控制回复传0，稍后重试。 */
int net_console_write(uint32_t session, const void *data, size_t length, int drop);
uint32_t net_console_dropped_bytes(void);

#endif
