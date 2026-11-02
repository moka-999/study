#include "pc_tester_info.h"
#include "flash_bsp.h"
#include "board_storage_config.h"
#include <string.h>

int pc_tester_info_load(fru_info_t *fru)
{
    if (fru == NULL) return -1;
#if !BOARD_W25_PRESENT
    /* 未焊W25的首板仅模拟信息响应；不创建记录，不伪造序列号/站点。 */
    fru_info_init(fru);
    _Static_assert(sizeof(TESTER_NAME) <= sizeof(fru->tester_name), "名称过长");
    _Static_assert(sizeof(TESTER_PRODUCT_NAME) <= sizeof(fru->product_name), "产品名过长");
    _Static_assert(sizeof(TESTER_HARDWARE_REVISION) <= sizeof(fru->hardware_revision), "版本过长");
    _Static_assert(sizeof(TESTER_BOARD_NAME) <= sizeof(fru->test_board), "板名过长");
    memcpy(fru->tester_name, TESTER_NAME, sizeof(TESTER_NAME));
    memcpy(fru->product_name, TESTER_PRODUCT_NAME, sizeof(TESTER_PRODUCT_NAME));
    memcpy(fru->hardware_revision, TESTER_HARDWARE_REVISION, sizeof(TESTER_HARDWARE_REVISION));
    memcpy(fru->test_board, TESTER_BOARD_NAME, sizeof(TESTER_BOARD_NAME));
    return 0;
#else
    if (flash_transaction_begin(1000U) != 0) return -1;
    int result = fru_info_load(&flash_if, fru, NULL);
    flash_transaction_end();
    return result == FLASH_STORE_OK ? 0 : -1;
#endif
}
