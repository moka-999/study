/*
 * ===================================================================
 * W25Q256 Flash BSP — flash_if_t 实例
 * ===================================================================
 * write() 自动 Read-Modify-Write 保护扇区数据
 * 用法: flash_if.init(&flash_if); flash_if.read(&flash_if, ...);
 * ===================================================================
 */
#ifndef __FLASH_BSP_H
#define __FLASH_BSP_H
#include "hal_if.h"

extern flash_if_t flash_if;

#endif
