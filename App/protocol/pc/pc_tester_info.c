#include "pc_tester_info.h"

#include "flash_bsp.h"
#include "board_identity.h"

#include <stddef.h>
#include <string.h>

#define TESTER_PRODUCT_NAME        "LPDDR4/X_200B"
#define TESTER_HARDWARE_REVISION   "D00"
#define TESTER_BOARD_NAME          "RK3588_test"

static int s_flash_initialized;

/* 只补空白字段；旧B1的产品、硬件版本等身份不能被默认常量改写。 */
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

    result = fill_missing_text(fru->product_name, sizeof(fru->product_name),
                               TESTER_PRODUCT_NAME);
    if (result < 0) return -1;
    changed |= result;

    result = fill_missing_text(fru->tester_name, sizeof(fru->tester_name),
                               fru->product_name);
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

int pc_tester_info_load(fru_info_t *fru)
{
    uint32_t generation = 0U;
    int load_result;
    int legacy_result;

    if (!fru) return -1;

    /*
     * ponytail: 当前正式固件只有单个TCP任务访问FRU，因此不引入互斥锁；
     * 后续若增加量产写入任务，必须在flash_if外层增加同一把锁。
     */
    if (!s_flash_initialized) {
        if (flash_if.init(&flash_if) != IF_OK) return -1;
        s_flash_initialized = 1;
    }

    load_result = fru_info_load(&flash_if, fru, &generation);
    /* 最新有效记录为v2时直接读取，不用编译期常量覆盖它。 */
    if (load_result == FLASH_STORE_OK) return 0;
    if (load_result != FLASH_STORE_ERR_NOT_FOUND &&
        load_result != FLASH_STORE_ERR_CORRUPT &&
        load_result != FLASH_STORE_ERR_LEGACY) {
        /* 包括UNSUPPORTED：有效未知格式必须保留，不能落入初始化写入。 */
        return -1;
    }

    /*
     * 只有空白/损坏/已知旧格式才进入一次性初始化。LEGACY表示有效旧记录
     * 比现有v2更新或尚无v2，必须读旧记录迁移；兼容160/192字节v1，
     * 保留已有产品、硬件版本、序列号、MAC、IP、生产日期和站点标签。
     */
    legacy_result = fru_info_load_legacy_v1(&flash_if, fru, &generation);
    if (legacy_result != FLASH_STORE_OK) {
        if (legacy_result != FLASH_STORE_ERR_NOT_FOUND &&
            legacy_result != FLASH_STORE_ERR_CORRUPT) {
            return -1;
        }
        fru_info_init(fru);
        /* 仅新建记录填入本板UID派生MAC；已有v2和旧格式迁移都保留原MAC。
         * 网络模式保持UNSET，继续使用B1既有默认IP，不猜测8槽的地址分配。 */
        memcpy(fru->mac_address, board_mac_address(), sizeof(fru->mac_address));
    }

    if (fill_missing_fields(fru) < 0) return -1;
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
