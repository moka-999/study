#include "boot_fru.h"
#include "board_storage_config.h"
#include <string.h>

_Static_assert(sizeof(TESTER_NAME) <= sizeof(((fru_info_t *)0)->tester_name), "名称过长");

/* 只补空白字段，不能把旧记录已有的板卡身份改成默认常量。 */
static int fill_missing_text(char *destination, size_t capacity, const char *source)
{
    const size_t length = strlen(source);

    if (destination[0] != '\0') return 0;
    if (length >= capacity) return -1;
    memset(destination, 0, capacity);
    memcpy(destination, source, length);
    return 1;
}

static int fill_missing_fields(fru_info_t *fru)
{
    int changed = 0;
    int result;

    result = fill_missing_text(fru->tester_name, sizeof(fru->tester_name),
                         TESTER_NAME);
    if (result < 0) return -1;
    changed |= result;

    result = fill_missing_text(fru->product_name, sizeof(fru->product_name),
                         TESTER_PRODUCT_NAME);
    if (result < 0) return -1;
    changed |= result;

    result = fill_missing_text(fru->hardware_revision,
                         sizeof(fru->hardware_revision),
                         TESTER_HARDWARE_REVISION);
    if (result < 0) return -1;
    changed |= result;

    result = fill_missing_text(fru->test_board, sizeof(fru->test_board),
                         TESTER_BOARD_NAME);
    if (result < 0) return -1;
    changed |= result;
    return changed;
}

int boot_fru_prepare(flash_if_t *flash, const uint8_t *mac)
{
    fru_info_t record;
    fru_info_t *fru = &record;
    uint32_t generation = 0U;
    int load_result;
    int legacy_result;

    if (flash == NULL) return -1;

    load_result = fru_info_load(flash, fru, &generation);
    if (load_result == FLASH_STORE_OK) {
        /* 仅迁移旧默认名称；其它身份字段和已定制名称原样保留。 */
        if (strcmp(fru->tester_name, "LPDDR4/X_200B") != 0) return 0;
        memset(fru->tester_name, 0, sizeof(fru->tester_name));
        memcpy(fru->tester_name, TESTER_NAME, sizeof(TESTER_NAME));
        if (fru_info_save(flash, fru, &generation) != FLASH_STORE_OK) return -1;
        /* 只更新较旧副本；放行App前必须重新读取持久化记录。 */
        return fru_info_load(flash, fru, &generation) == FLASH_STORE_OK ? 0 : -1;
    }
    if (load_result != FLASH_STORE_ERR_NOT_FOUND &&
        load_result != FLASH_STORE_ERR_CORRUPT &&
        load_result != FLASH_STORE_ERR_LEGACY) {
        /* 包括UNSUPPORTED：有效未知格式必须保留，不能落入默认初始化。 */
        return -1;
    }

    /*
     * 只有空白/损坏/已知旧格式才进入一次性初始化。LEGACY表示最新有效
     * 记录仍是v1，兼容160/192字节布局；保留已有身份、MAC和网络字段。
     */
    legacy_result = fru_info_load_legacy_v1(flash, fru, &generation);
    if (legacy_result != FLASH_STORE_OK) {
        if (legacy_result != FLASH_STORE_ERR_NOT_FOUND &&
            legacy_result != FLASH_STORE_ERR_CORRUPT) {
            return -1;
        }
        fru_info_init(fru);
        if (mac != NULL) memcpy(fru->mac_address, mac, sizeof(fru->mac_address));
    }

    if (fill_missing_fields(fru) < 0) return -1;
    if (fru_info_save(flash, fru, &generation) != FLASH_STORE_OK) {
        return -1;
    }
    /* 空白Flash或v1升级再写一次，确保A/B都是有效v2副本。 */
    if (fru_info_save(flash, fru, &generation) != FLASH_STORE_OK) {
        return -1;
    }

    /* 只有重新读到持久化记录才能启动App，而不是刚才构造的RAM临时值。 */
    return fru_info_load(flash, fru, &generation) == FLASH_STORE_OK ? 0 : -1;
}

