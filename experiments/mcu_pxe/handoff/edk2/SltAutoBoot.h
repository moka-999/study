#ifndef SLT_AUTO_BOOT_H
#define SLT_AUTO_BOOT_H

#include <Uefi.h>

/* 控制台就绪后调用一次；TRUE表示已接管授权会话，调用方须留在手动菜单。 */
BOOLEAN SltAutoBoot (VOID);

#endif
