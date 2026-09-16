/*
 * ===================================================================
 * W25Q256 Flash BSP — flash_if_t 实例
 * ===================================================================
 * write() 自动 Read-Modify-Write 保护扇区数据；相同内容不会重复擦写。
 * 注意：当前实现共用一个 4 KiB 缓冲，多任务访问时必须由上层串行化。
 * 用法: flash_if.init(&flash_if); flash_if.read(&flash_if, ...);
 * ===================================================================
 */
#ifndef __FLASH_BSP_H
#define __FLASH_BSP_H
#include "hal_if.h"

extern flash_if_t flash_if;

/** 创建唯一Flash事务锁，必须在调度前调用；0成功，失败不得创建Flash访问任务。 */
int flash_transaction_prepare(void);
/** 任务上下文获取事务锁并首次初始化QSPI；timeout_ms有限等待，0成功。 */
int flash_transaction_begin(uint32_t timeout_ms);
/** 仅由成功begin的任务调用；保护完整load/修改/save，不只是单次总线读写。 */
void flash_transaction_end(void);

#endif
