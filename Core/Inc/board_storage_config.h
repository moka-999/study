#ifndef BOARD_STORAGE_CONFIG_H
#define BOARD_STORAGE_CONFIG_H

/* 0：未焊W25，Boot跳过器件、PC使用下方常量；1：Boot维护FRU，App只读。
 * 当前验板目标未焊W25，默认0；已装器件板须改为1并重编完整Boot＋App。 */
#ifndef BOARD_W25_PRESENT
#define BOARD_W25_PRESENT 1
#endif
#if BOARD_W25_PRESENT != 0 && BOARD_W25_PRESENT != 1
#error "BOARD_W25_PRESENT只能为0或1"
#endif

#define TESTER_PRODUCT_NAME       "LPDDR4/X_200B"
#define TESTER_NAME               "JMCT-4000"
#define TESTER_HARDWARE_REVISION  "D00"
#define TESTER_BOARD_NAME         "RK3588_test"

/* 正式App不允许擦写W25；隔离Flash自检保留原有末尾64KiB测试入口。 */
#if defined(ENABLE_FLASH_SELF_TEST) && ENABLE_FLASH_SELF_TEST
#define APP_FLASH_WRITABLE 1
#else
#define APP_FLASH_WRITABLE 0
#endif
#endif
