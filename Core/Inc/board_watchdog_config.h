#ifndef BOARD_WATCHDOG_CONFIG_H
#define BOARD_WATCHDOG_CONFIG_H

/* 0：验板时不启动IWDG、不创建喂狗任务；1：启用原有四任务存活监督。
 * 修改后须重新编译并烧录。仅适用于IWDG由软件启动的Option Bytes配置；
 * 不能停止旧固件已经启动的看门狗，也不能替代下载期间的调试冻结。 */
#ifndef BOARD_IWDG_ENABLE
#define BOARD_IWDG_ENABLE 0U
#endif

#if BOARD_IWDG_ENABLE != 0U && BOARD_IWDG_ENABLE != 1U
#error "BOARD_IWDG_ENABLE只能为0或1"
#endif

#endif
