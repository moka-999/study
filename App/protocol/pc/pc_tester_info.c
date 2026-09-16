#include "pc_tester_info.h"

#include "flash_bsp.h"

#include <stddef.h>
#include <string.h>

#define TESTER_PRODUCT_NAME        "LPDDR4/X_200B"
#define TESTER_HARDWARE_REVISION   "D00"
#define TESTER_BOARD_NAME          "RK3588_test"

/* 返回1表示内容发生变化，0表示Flash中的值已经与默认值一致。 */
static int update_text(char *destination, size_t capacity, const char *source)
{
    const size_t length = strlen(source);

    if (length >= capacity) return -1;
    if (strcmp(destination, source) == 0) return 0;
    memset(destination, 0, capacity);
    memcpy(destination, source, length);
    return 1;
}

static int update_confirmed_fields(fru_info_t *fru)
{
    int changed = 0;
    int result;

    result = update_text(fru->tester_name, sizeof(fru->tester_name),
                         TESTER_PRODUCT_NAME);
    if (result < 0) return -1;
    changed |= result;

    result = update_text(fru->product_name, sizeof(fru->product_name),
                         TESTER_PRODUCT_NAME);
    if (result < 0) return -1;
    changed |= result;

    result = update_text(fru->hardware_revision,
                         sizeof(fru->hardware_revision),
                         TESTER_HARDWARE_REVISION);
    if (result < 0) return -1;
    changed |= result;

    result = update_text(fru->test_board, sizeof(fru->test_board),
                         TESTER_BOARD_NAME);
    if (result < 0) return -1;
    changed |= result;
    return changed;
}

static int load_locked(fru_info_t *fru)
{
    uint32_t generation = 0U;
    int load_result;
    int legacy_result;

    if (!fru) return -1;

    load_result = fru_info_load(&flash_if, fru, &generation);
    /* 有效v2就是唯一数据源：查询路径不用编译期常量覆盖它。 */
    if (load_result == FLASH_STORE_OK) return 0;
    if (load_result != FLASH_STORE_ERR_NOT_FOUND &&
        load_result != FLASH_STORE_ERR_CORRUPT) {
        return -1;
    }

    /*
     * 只有空白/损坏/旧格式才进入一次性初始化。迁移保留序列号、
     * MAC、IP、生产日期和站点标签，再补齐v2新字段。
     */
    legacy_result = fru_info_load_legacy_v1(&flash_if, fru, &generation);
    if (legacy_result != FLASH_STORE_OK) {
        if (legacy_result != FLASH_STORE_ERR_NOT_FOUND &&
            legacy_result != FLASH_STORE_ERR_CORRUPT) {
            return -1;
        }
        fru_info_init(fru);
    }

    if (update_confirmed_fields(fru) < 0) return -1;
    if (fru_info_save(&flash_if, fru, &generation) != FLASH_STORE_OK) {
        return -1;
    }
    /* 空白Flash或v1升级再写一次，确保A/B都是有效v2副本。 */
    if (fru_info_save(&flash_if, fru, &generation) != FLASH_STORE_OK) {
        return -1;
    }

    /* 响应必须来自持久化后的实际记录，而不是刚才构造的RAM临时值。 */
    return fru_info_load(&flash_if, fru, &generation) == FLASH_STORE_OK ? 0 : -1;
}

int pc_tester_info_load(fru_info_t *fru)
{
    if (fru == NULL || flash_transaction_begin(1000U) != 0) return -1;
    int result = load_locked(fru);
    flash_transaction_end();
    return result;
}
