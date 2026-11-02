/*
 * ============================================================================
 * W25Q256 持久化布局与设备信息记录
 * ============================================================================
 *
 * 设计原则：
 *   1. 本模块提供版本/FRU记录接口；正式业务仅使用FRU，不自动写固件版本。
 *   2. 每类关键记录各占两个独立 4 KiB 扇区。更新时只改旧副本，另一个
 *      有效副本始终保留，因此写入中断电后仍可恢复最近一次完整记录。
 *   3. 所有整数按 STM32H743 的小端格式存储；字符数组使用 UTF-8/ASCII，
 *      有效文本必须以 '\0' 结尾。
 *   4. Flash 访问由调用者串行化。底层 flash_if 和扇区 RMW 缓冲不可重入。
 *
 * 32 MiB 空间分配：
 *   0x00000000 - 0x0000FFFF  元数据区（版本 A/B、FRU A/B、后续扩展）
 *   0x00010000 - 0x0040FFFF  固件扩展预留区（4 MiB，当前不读写或擦除）
 *   0x00410000 - 0x00C0FFFF  RK固件空间A（8 MiB，仅预留）
 *   0x00C10000 - 0x0140FFFF  RK固件空间B（8 MiB，仅预留）
 *   0x01410000 - 0x01FEFFFF  预留数据区（日志/标定等用途尚未冻结）
 *   0x01FF0000 - 0x01FFFFFF  开发与产测自检区（64 KiB，可擦除）
 * ============================================================================
 */
#ifndef FLASH_LAYOUT_H
#define FLASH_LAYOUT_H

#include "hal_if.h"
#include <stddef.h>
#include <stdint.h>

/* W25Q256 固定容量与最小擦除单位。这里重复声明是为了让布局可独立审查。 */
#define FLASH_LAYOUT_TOTAL_SIZE             0x02000000UL
#define FLASH_LAYOUT_SECTOR_SIZE            0x00001000UL

#define FLASH_LAYOUT_METADATA_BASE          0x00000000UL
#define FLASH_LAYOUT_METADATA_SIZE          0x00010000UL

/* B1固定使用生产元数据地址，不提供V2自检镜像的地址重映射。 */
#define FLASH_LAYOUT_VERSION_SLOT_A         0x00000000UL
#define FLASH_LAYOUT_VERSION_SLOT_B         0x00001000UL
#define FLASH_LAYOUT_FRU_SLOT_A             0x00002000UL
#define FLASH_LAYOUT_FRU_SLOT_B             0x00003000UL

/* 保留原地址，不重新分配或自动清除其中已有内容。 */
#define FLASH_LAYOUT_RESERVED_FIRMWARE_BASE 0x00010000UL
#define FLASH_LAYOUT_RESERVED_FIRMWARE_SIZE 0x00400000UL

/*
 * RK的两个独立固件存放空间，每块8 MiB；此处只保留地址，不读写或擦除。
 * A/B不表示RK双槽回滚，也不预先绑定SPL/EDK；记录格式与传输协议另行确定。
 */
#define FLASH_LAYOUT_RK_FIRMWARE_SLOT_A_BASE 0x00410000UL
#define FLASH_LAYOUT_RK_FIRMWARE_SLOT_B_BASE 0x00C10000UL
#define FLASH_LAYOUT_RK_FIRMWARE_SLOT_SIZE   0x00800000UL

#define FLASH_LAYOUT_RESERVED_DATA_BASE     0x01410000UL
#define FLASH_LAYOUT_RESERVED_DATA_SIZE     0x00BE0000UL

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

/*
 * 固件版本载荷（固定 76 字节）。字段严格保持精简，不保存构建编号、
 * Git提交号或工作区状态。内建结构的image_length/image_crc32为0；
 * 版本记录接口用于介质自检，正式App启动和PC查询不读写此记录。
 */
typedef struct {
    uint16_t schema_version;
    uint16_t struct_size;
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t version_patch;
    uint32_t image_length;
    uint32_t image_crc32;
    char     build_time_utc[24];
    char     build_id[16];
    char     target[16];
} firmware_version_info_t;

#define FRU_INFO_SCHEMA_VERSION             2U

typedef enum {
    FRU_NETWORK_MODE_UNSET  = 0,  /* 未写入网络配置，使用板型固件默认值 */
    FRU_NETWORK_MODE_STATIC = 1,  /* 使用 configured_ipv4/netmask/gateway */
    FRU_NETWORK_MODE_DHCP   = 2,  /* 地址由 DHCP 获取，配置地址字段忽略 */
} fru_network_mode_t;

/*
 * FRU v2 基础字段（固定 224 字节）。
 * 当前字段覆盖产品名、序列号、硬件版本、生产日期、MAC、网络配置和
 * 站点标签，并新增PC协议明确需要的测试仪名称和测试板名称。网络字段是
 * 持久化配置，不等同于运行时实际地址；
 * PC 查询时必须同时读取 lwIP 当前地址。
 * MAC全0表示未配置。以后新增字段只能占用reserved或提升schema，不能改变
 * v2已有字段的偏移。
 */
typedef struct {
    uint16_t schema_version;
    uint16_t struct_size;
    char     product_name[32];
    char     serial_number[32];
    char     hardware_revision[16];
    char     manufacturing_date[16];
    uint8_t  mac_address[6];
    uint8_t  network_mode;
    uint8_t  configured_ipv4[4];
    uint8_t  configured_netmask[4];
    uint8_t  configured_gateway[4];
    uint16_t service_port;
    char     site_label[16];
    char     tester_name[32];
    char     test_board[32];
    uint8_t  reserved[22];
} fru_info_t;

typedef enum {
    FLASH_STORE_OK                = 0,
    FLASH_STORE_ERR_PARAM         = -1,
    FLASH_STORE_ERR_IO            = -2,
    FLASH_STORE_ERR_NOT_FOUND     = -3,
    FLASH_STORE_ERR_CORRUPT       = -4,
    FLASH_STORE_ERR_VERIFY        = -5,
    FLASH_STORE_ERR_UNSUPPORTED   = -6, /* CRC有效但格式未知；禁止默认初始化/覆盖。 */
    FLASH_STORE_ERR_LEGACY        = -7, /* 最新有效FRU仍为旧格式，须读取旧格式后迁移。 */
} flash_store_status_t;

/*
 * 当前固件的编译期身份，供调试定位与介质自检；不会自动写入W25。
 * PC测试仪信息读取FRU，testerVersion表示硬件版本，不使用此结构。
 */
const firmware_version_info_t *firmware_version_current(void);

/* 初始化一个空白但格式有效的 FRU，供生产写入工具逐项填写。 */
void fru_info_init(fru_info_t *fru);

/* 双副本先逐槽校验CRC和字段，再比较代次；未知格式返回UNSUPPORTED。
 * firmware_version_load还可把旧96字节记录只读转换为当前76字节输出。
 * fru_info_load遇最新有效记录为160/192字节旧格式返回LEGACY，不冒用较旧v2。 */
int firmware_version_load(flash_if_t *flash, firmware_version_info_t *info,
                          uint32_t *generation);
int firmware_version_save(flash_if_t *flash, const firmware_version_info_t *info,
                          uint32_t *generation);
int fru_info_load(flash_if_t *flash, fru_info_t *fru, uint32_t *generation);
int fru_info_save(flash_if_t *flash, const fru_info_t *fru,
                  uint32_t *generation);

/*
 * 仅供持久化格式升级使用：读取旧160字节或B1旧192字节FRU v1，
 * 按generation选择最新有效副本并转换为RAM中的v2结构，不写Flash。
 * 保留身份和网络字段；192字节格式中mac_valid=0按未配置MAC处理。
 */
int fru_info_load_legacy_v1(flash_if_t *flash, fru_info_t *fru,
                            uint32_t *generation);

/* IEEE CRC-32，供记录校验与介质自检使用。 */
uint32_t flash_crc32(const uint8_t *data, uint32_t length);

_Static_assert(sizeof(flash_record_header_t) == 32U, "flash record header size changed");
_Static_assert(sizeof(firmware_version_info_t) == 76U, "firmware version layout changed");
_Static_assert(sizeof(fru_info_t) == 224U, "FRU v2 layout changed");
/* 持久化ABI不能只看总长；锁定新字段与保留区的实际偏移。 */
_Static_assert(offsetof(fru_info_t, tester_name) == 138U,
               "FRU tester_name offset changed");
_Static_assert(offsetof(fru_info_t, test_board) == 170U,
               "FRU test_board offset changed");
_Static_assert(offsetof(fru_info_t, reserved) == 202U,
               "FRU reserved offset changed");
/* 按地址连续性与扇区对齐锁定预留边界，防止后续扩展覆盖既有MCU或自检区。 */
_Static_assert(FLASH_LAYOUT_METADATA_BASE + FLASH_LAYOUT_METADATA_SIZE ==
               FLASH_LAYOUT_RESERVED_FIRMWARE_BASE, "元数据区必须与固件扩展预留区相邻");
_Static_assert(FLASH_LAYOUT_RESERVED_FIRMWARE_BASE + FLASH_LAYOUT_RESERVED_FIRMWARE_SIZE ==
               FLASH_LAYOUT_RK_FIRMWARE_SLOT_A_BASE, "RK空间不得覆盖固件扩展预留区");
_Static_assert(FLASH_LAYOUT_RK_FIRMWARE_SLOT_A_BASE + FLASH_LAYOUT_RK_FIRMWARE_SLOT_SIZE ==
               FLASH_LAYOUT_RK_FIRMWARE_SLOT_B_BASE, "两个RK固件空间必须相邻");
_Static_assert(FLASH_LAYOUT_RK_FIRMWARE_SLOT_B_BASE + FLASH_LAYOUT_RK_FIRMWARE_SLOT_SIZE ==
               FLASH_LAYOUT_RESERVED_DATA_BASE, "RK空间必须与余下预留区相邻");
_Static_assert(FLASH_LAYOUT_RK_FIRMWARE_SLOT_SIZE == 8UL * 1024UL * 1024UL,
               "每个RK固件空间必须为8 MiB");
_Static_assert(FLASH_LAYOUT_RK_FIRMWARE_SLOT_A_BASE % FLASH_LAYOUT_SECTOR_SIZE == 0UL &&
               FLASH_LAYOUT_RK_FIRMWARE_SLOT_B_BASE % FLASH_LAYOUT_SECTOR_SIZE == 0UL &&
               FLASH_LAYOUT_RK_FIRMWARE_SLOT_SIZE % FLASH_LAYOUT_SECTOR_SIZE == 0UL,
               "RK固件空间必须按擦除扇区对齐");
_Static_assert(FLASH_LAYOUT_RESERVED_DATA_BASE + FLASH_LAYOUT_RESERVED_DATA_SIZE ==
               FLASH_LAYOUT_SELF_TEST_BASE, "reserved data range is inconsistent");
_Static_assert(FLASH_LAYOUT_SELF_TEST_BASE + FLASH_LAYOUT_SELF_TEST_SIZE ==
               FLASH_LAYOUT_TOTAL_SIZE, "W25Q256 layout does not cover 32 MiB");

#endif /* FLASH_LAYOUT_H */
