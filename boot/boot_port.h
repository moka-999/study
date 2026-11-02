#ifndef BOOT_PORT_H
#define BOOT_PORT_H
#include "hal_if.h"
#include <stdint.h>

/* 只接受复位后的HSI 64MHz；不启动RTOS/看门狗，不依赖外部晶振。 */
int boot_port_init(void);
/* 此接口的写范围仅为FRU两个4KiB扇区，不能写版本或RK固件预留区。 */
extern flash_if_t boot_flash;
extern volatile uint32_t boot_diag[4];
/* 关闭Boot外设、切换VTOR/MSP并进入App，调用前已验证向量。 */
_Noreturn void boot_jump(uint32_t stack, uint32_t entry);
#endif
