#ifndef BOOT_RECORD_H
#define BOOT_RECORD_H

#include "ota_install.h"

#define BOOT_JOURNAL_BASE UINT32_C(0x00004000)
#define BOOT_JOURNAL_SECTOR 4096U
#define BOOT_RECORD_BYTES 256U
#define BOOT_SLOT_NONE UINT32_MAX

/** NORMAL为已确认版本；READY为下载提交；INSTALLING目标尚无效；TRIAL待健康确认。 */
typedef enum {
    BOOT_NORMAL = 0, BOOT_READY, BOOT_INSTALLING, BOOT_TRIAL, BOOT_MAINTENANCE
} boot_phase_t;

/**
 * 启动记录是选槽的唯一依据，不用版本号高低猜测当前槽。
 * active/previous为0(A1)、1(A2)；previous还允许BOOT_SLOT_NONE。
 * stage为0当前版本、1回退版本、2稳定版、3维护；failures是本阶段未确认启动次数。
 * pending在跳转前置1，App持续健康30秒后清零；epoch绑定该次启动，拒绝旧确认。
 * images保存实际写入各槽的CRC/版本；valid_mask清零的描述不允许启动。
 */
typedef struct {
    uint32_t generation;
    uint32_t phase, active, previous, valid_mask;
    uint32_t pending, stage, failures, install_tries;
    uint32_t package_bytes, package_crc, epoch, last_error;
    ota_image_t images[2];
} boot_record_t;

/** 原始NOR操作：program只允许1变0，不得内部擦除/RMW；所有回调0成功。 */
typedef struct {
    void *context;
    int (*read)(void *, uint32_t, uint8_t *, uint32_t);
    int (*program)(void *, uint32_t, const uint8_t *, uint32_t);
    int (*erase)(void *, uint32_t);
} boot_store_t;

enum {
    BOOT_STORE_OK = 0, BOOT_STORE_EMPTY = 1,
    BOOT_STORE_INVALID = -1, BOOT_STORE_IO = -2, BOOT_STORE_STALE = -3
};

/** 扫描两个4KiB扇区，返回最新完整记录；区分全空白、损坏、IO异常，失败清零输出。 */
int boot_record_load(const boot_store_t *store, boot_record_t *record);

/**
 * 追加并读回验证，成功才更新record.generation。输入代数必须仍是最新值。
 * 写满后仅擦另一扇区，先写252字节正文/CRC，最后写4字节提交标记。
 * 中途掉电最多丢本次修改；调用方仍须串行化整个load/修改/save事务。
 */
int boot_record_save(const boot_store_t *store, boot_record_t *record);

/** 以下仅修改RAM，调用者必须save成功才可执行切槽/跳转等硬件动作。 */
void boot_record_failed_start(boot_record_t *record);
void boot_record_fallback(boot_record_t *record);
int boot_record_confirm(boot_record_t *record, uint32_t slot_base, uint32_t epoch);
uint32_t boot_slot_base(uint32_t slot);

#endif
