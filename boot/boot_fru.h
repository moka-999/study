#ifndef BOOT_FRU_H
#define BOOT_FRU_H
#include "flash_layout.h"

/* 初始化空白/损坏记录，迁移已知旧格式；保留定制信息和未知有效格式。
 * mac为新建记录使用的6字节地址，可为NULL；0表示持久化回读通过，-1失败。
 * 仅Boot调用，不能从App查询路径调用。重复启动且记录有效时不写入。 */
int boot_fru_prepare(flash_if_t *flash, const uint8_t *mac);
#endif
