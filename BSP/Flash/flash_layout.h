/*
 * ============================================================================
 * W25Q256 持久化布局与设备信息记录
 * ============================================================================
 *
 * 设计原则：
 *   1. 当前版本只实现版本信息和 FRU，不实现 OTA 下载、切换或回滚。
 *   2. 每类关键记录各占两个独立 4 KiB 扇区。更新时只改旧副本，另一个
 *      有效副本始终保留，因此写入中断电后仍可恢复最近一次完整记录。
 *   3. 所有整数按 STM32H743 的小端格式存储；字符数组使用 UTF-8/ASCII，
 *      有效文本必须以 '\0' 结尾。
 *   4. Flash 访问由调用者串行化。底层 flash_if 和扇区 RMW 缓冲不可重入。
 *
 * 32 MiB 空间分配：
 *   0x00000000 - 0x0000FFFF  元数据区（版本 A/B、FRU A/B、后续扩展）
 *   0x00010000 - 0x0020FFFF  预留 OTA 镜像槽 A（2 MiB，当前不使用）
 *   0x00210000 - 0x0040FFFF  预留 OTA 镜像槽 B（2 MiB，当前不使用）
 *   0x00410000 - 0x01FEFFFF  预留数据区（日志/标定等用途尚未冻结）
 *   0x01FF0000 - 0x01FFFFFF  开发与产测自检区（64 KiB，可擦除）
 * ============================================================================
 */
#ifndef FLASH_LAYOUT_H
#define FLASH_LAYOUT_H

#include "hal_if.h"
#include <stdint.h>

/* W25Q256 固定容量与最小擦除单位。这里重复声明是为了让布局可独立审查。 */
#define FLASH_LAYOUT_TOTAL_SIZE             0x02000000UL
#define FLASH_LAYOUT_SECTOR_SIZE            0x00001000UL

#define FLASH_LAYOUT_METADATA_BASE          0x00000000UL
#define FLASH_LAYOUT_METADATA_SIZE          0x00010000UL

#define FLASH_LAYOUT_VERSION_SLOT_A         0x00000000UL
#define FLASH_LAYOUT_VERSION_SLOT_B         0x00001000UL
#define FLASH_LAYOUT_FRU_SLOT_A             0x00002000UL
#define FLASH_LAYOUT_FRU_SLOT_B             0x00003000UL

/* 只冻结地址，不提供 OTA 行为。每槽 2 MiB，可容纳当前最大 1920 KiB App。 */
#define FLASH_LAYOUT_OTA_SLOT_A_BASE        0x00010000UL
#define FLASH_LAYOUT_OTA_SLOT_B_BASE        0x00210000UL
#define FLASH_LAYOUT_OTA_SLOT_SIZE          0x00200000UL

#define FLASH_LAYOUT_RESERVED_DATA_BASE     0x00410000UL
#define FLASH_LAYOUT_RESERVED_DATA_SIZE     0x01BE0000UL

#define FLASH_LAYOUT_SELF_TEST_BASE         0x01FF0000UL
#define FLASH_LAYOUT_SELF_TEST_SIZE         0x00010000UL

/* 记录公共格式。Magic 的内存字节序为 ASCII "B1RC"。 */
#define FLASH_RECORD_MAGIC                  0x43523142UL
#define FLASH_RECORD_FORMAT_VERSION         1U

typedef enum {
    FLASH_RECORD_TYPE_FIRMWARE_VERSION = 1,
    FLASH_RECORD_TYPE_FRU              = 2,
} flash_record_type_t;

/*
 * 每个副本扇区开头的 32 字节公共头。
 * header_crc32 覆盖它之前的 20 字节；payload_crc32 只覆盖有效载荷。
 * reserved 必须写 0xFF，供后续格式扩展使用。
 */
typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t record_type;
    uint32_t generation;
    uint32_t payload_length;
    uint32_t payload_crc32;
    uint32_t header_crc32;
    uint8_t  reserved[8];
} flash_record_header_t;

#define FIRMWARE_VERSION_SCHEMA_VERSION     1U
#define FIRMWARE_VERSION_FLAG_IMAGE_VALID   (1U << 0)
#define FIRMWARE_VERSION_FLAG_GIT_DIRTY     (1U << 1)

/*
 * 固件版本载荷（固定 96 字节）。
 * image_length/image_crc32 由固件打包步骤填写；若尚未打包，二者为 0 且
 * FIRMWARE_VERSION_FLAG_IMAGE_VALID 不置位，避免把占位值误当成校验结果。
 */
typedef struct {
    uint16_t schema_version;
    uint16_t struct_size;
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t version_patch;
    uint16_t flags;
    uint32_t build_number;
    uint32_t image_length;
    uint32_t image_crc32;
    char     build_time_utc[24];
    char     git_commit[16];
    char     build_id[16];
    char     target[16];
} firmware_version_info_t;

#define FRU_INFO_SCHEMA_VERSION             1U

typedef enum {
    FRU_NETWORK_MODE_UNSET  = 0,  /* 未写入网络配置，使用板型固件默认值 */
    FRU_NETWORK_MODE_STATIC = 1,  /* 使用 configured_ipv4/netmask/gateway */
    FRU_NETWORK_MODE_DHCP   = 2,  /* 地址由 DHCP 获取，配置地址字段忽略 */
} fru_network_mode_t;

/*
 * FRU v1 基础字段（固定 192 字节）。
 * 当前字段覆盖产品名、料号、序列号、硬件版本、生产日期、网络配置、
 * 可选 MAC 和站点标签。网络字段是持久化配置，不等同于运行时实际地址；
 * PC 查询时必须同时读取 lwIP 当前地址。
 * 产品字段尚未最终冻结；以后新增字段只能占用 reserved 或提升 schema，
 * 不能改变 v1 已有字段的偏移。
 */
typedef struct {
    uint16_t schema_version;
    uint16_t struct_size;
    char     product_name[32];
    char     part_number[32];
    char     serial_number[32];
    char     hardware_revision[16];
    char     manufacturing_date[16];
    uint8_t  mac_address[6];
    uint8_t  mac_valid;
    uint8_t  network_mode;
    uint8_t  configured_ipv4[4];
    uint8_t  configured_netmask[4];
    uint8_t  configured_gateway[4];
    uint16_t service_port;
    char     site_label[16];
    uint8_t  reserved[22];
} fru_info_t;

typedef enum {
    FLASH_STORE_OK                = 0,
    FLASH_STORE_ERR_PARAM         = -1,
    FLASH_STORE_ERR_IO            = -2,
    FLASH_STORE_ERR_NOT_FOUND     = -3,
    FLASH_STORE_ERR_CORRUPT       = -4,
    FLASH_STORE_ERR_VERIFY        = -5,
} flash_store_status_t;

/* 当前固件内建版本。镜像长度/CRC 在生成打包清单后才有效。 */
const firmware_version_info_t *firmware_version_current(void);

/* 初始化一个空白但格式有效的 FRU，供生产写入工具逐项填写。 */
void fru_info_init(fru_info_t *fru);

/* 双副本读写：load 自动选择 CRC 有效且 generation 最新的副本。 */
int firmware_version_load(flash_if_t *flash, firmware_version_info_t *info,
                          uint32_t *generation);
int firmware_version_save(flash_if_t *flash, const firmware_version_info_t *info,
                          uint32_t *generation);
int fru_info_load(flash_if_t *flash, fru_info_t *fru, uint32_t *generation);
int fru_info_save(flash_if_t *flash, const fru_info_t *fru,
                  uint32_t *generation);

/* IEEE CRC-32，供离线打包工具和后续 OTA 镜像校验共用。 */
uint32_t flash_crc32(const uint8_t *data, uint32_t length);

_Static_assert(sizeof(flash_record_header_t) == 32U, "flash record header size changed");
_Static_assert(sizeof(firmware_version_info_t) == 96U, "firmware version layout changed");
_Static_assert(sizeof(fru_info_t) == 192U, "FRU v1 layout changed");
_Static_assert(FLASH_LAYOUT_OTA_SLOT_A_BASE + FLASH_LAYOUT_OTA_SLOT_SIZE ==
               FLASH_LAYOUT_OTA_SLOT_B_BASE, "OTA slots must be adjacent");
_Static_assert(FLASH_LAYOUT_RESERVED_DATA_BASE + FLASH_LAYOUT_RESERVED_DATA_SIZE ==
               FLASH_LAYOUT_SELF_TEST_BASE, "reserved data range is inconsistent");
_Static_assert(FLASH_LAYOUT_SELF_TEST_BASE + FLASH_LAYOUT_SELF_TEST_SIZE ==
               FLASH_LAYOUT_TOTAL_SIZE, "W25Q256 layout does not cover 32 MiB");

#endif /* FLASH_LAYOUT_H */
