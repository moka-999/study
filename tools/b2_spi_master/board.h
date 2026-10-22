#ifndef MASTER_BOARD_H
#define MASTER_BOARD_H

#include <stddef.h>
#include <stdint.h>

int master_board_init(void);
void console_write(const char *text);
/* 返回一个接收字节；-1无数据，-2串口丢字节（调用者须废弃当前命令行）。 */
int console_read(void);
uint32_t master_millis(void);
void master_delay(uint32_t ms);
void master_select(int selected);
/* 同步收发原始字节，1成功、0外设错误；调用者负责片选及恢复高电平。 */
int master_transfer(const uint8_t *tx, uint8_t *rx, size_t length);

#endif
