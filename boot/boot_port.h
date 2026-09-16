#ifndef BOOT_PORT_H
#define BOOT_PORT_H

#include "boot_record.h"

/** 本端口仅对应阿波罗V2的PB2/PB6/PF6..9；B1不得直接烧用。 */
int boot_port_init(void);
uint32_t boot_millis(void);
void boot_watchdog_feed(void);
_Noreturn void boot_maintenance(uint32_t error);
_Noreturn void boot_jump(uint32_t base);

/** NOR原始操作：32MiB读白名单；写/擦仅版本、启动日志、下载区，FRU和稳定区禁止改写。 */
int boot_nor_read(void *context, uint32_t address, uint8_t *data, uint32_t bytes);
int boot_nor_program(void *context, uint32_t address, const uint8_t *data, uint32_t bytes);
int boot_nor_erase(void *context, uint32_t address);

/** INSTALLING记录保存成功后授权唯一目标；其他内部Flash范围不可写，包括B本身。 */
void boot_allow_target(uint32_t base);
int boot_flash_erase(void *context, uint32_t address);
int boot_flash_program(void *context, uint32_t address, const uint8_t *word);
int boot_flash_read(void *context, uint32_t address, uint8_t *data, uint32_t bytes);

/** SWD诊断地址必须从本次ELF读取；magic、阶段、错误、JEDEC、目标、启动代数、跳转地址、ECC次数。 */
extern volatile uint32_t boot_diag[8];

#endif
