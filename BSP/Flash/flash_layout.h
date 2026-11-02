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

/*
 * 独立硬件自检镜像把四个副本临时映射到末尾自检区，绝不擦正式元数据。
 * 正式构建未定义 ENABLE_FLASH_SELF_TEST，地址仍固定在 Flash 起始处。
 */
#if defined(ENABLE_FLASH_SELF_TEST) && ENABLE_FLASH_SELF_TEST
#define FLASH_LAYOUT_VERSION_SLOT_A         0x01FF0000UL
#define FLASH_LAYOUT_VERSION_SLOT_B         0x01FF1000UL
#define FLASH_LAYOUT_FRU_SLOT_A             0x01FF2000UL
#define FLASH_LAYOUT_FRU_SLOT_B             0x01FF3000UL
#else
#define FLASH_LAYOUT_VERSION_SLOT_A         0x00000000UL
#define FLASH_LAYOUT_VERSION_SLOT_B         0x00001000UL
#define FLASH_LAYOUT_FRU_SLOT_A             0x00002000UL
#define FLASH_LAYOUT_FRU_SLOT_B             0x00003000UL
#endif

/* 取消MCU升级后保留原地址，不重新分配或自动清除其中已有内容。 */
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
    FLASH_STORE_ERR_UNSUPPORTED   = -6, /* 有效未知格式：拒绝读取及覆盖 */
    FLASH_STORE_ERR_LEGACY        = -7, /* 最新有效FRU为旧格式，须显式迁移 */
} flash_store_status_t;

/**
 * @brief 获取当前镜像编译期身份，供调试定位与介质自检。
 * @return 静态只读结构，整个运行期有效，不可修改或释放。
 * @note 不读写Flash，image_length/image_crc32固定为0。PC测试仪信息
 *       读取FRU；testerVersion表示硬件版本，不使用此结构组帧。
 */
const firmware_version_info_t *firmware_version_current(void);

/**
 * @brief 初始化空白但格式有效的FRU v2，供后续逐项填写。
 * @param[out] fru 输出结构；NULL时不操作。
 * @note 清零字段、设置schema/size并将reserved填0xFF；不读写Flash。
 */
void fru_info_init(fru_info_t *fru);

/**
 * @brief 从A/B副本加载CRC及字段合法、generation最新的固件版本记录。
 * @param[in,out] flash 已初始化的同步Flash接口，只要求read回调。
 * @param[out] info 非NULL输出结构；仅返回FLASH_STORE_OK时有效。
 * @param[out] generation 可为NULL；选中记录的代数，不是输入期望版本。
 * @return FLASH_STORE_OK成功；ERR_PARAM参数无效；ERR_IO无可用副本且访问
 *         失败；ERR_NOT_FOUND两副本空白；ERR_CORRUPT记录损坏；
 *         ERR_UNSUPPORTED存在有效未知格式，禁止覆盖或默认初始化。
 * @note 兼容76/96字节版本记录，旧记录仅在RAM中转换；不写Flash。
 *       仅成功时使用输出，完整事务由调用方串行化。
 */
int firmware_version_load(flash_if_t *flash, firmware_version_info_t *info,
                          uint32_t *generation);

/**
 * @brief 将固件版本写入较旧或无效副本，并回读验证。
 * @param[in,out] flash 已初始化且提供read/write回调的Flash接口。
 * @param[in] info 非NULL、schema/size/字段合法的记录，返回前保持有效。
 * @param[out] generation 可为NULL；成功时返回新代数，失败保持原值。
 * @return FLASH_STORE_OK成功；ERR_PARAM参数/记录格式无效；ERR_IO读写失败；
 *         ERR_VERIFY写入后回读校验失败；ERR_UNSUPPORTED有效未知格式禁止覆盖。
 * @note 同步擦写，调用方须串行化完整事务。只更新一个副本并保留另一有效
 *       副本；失败可能已改变目标扇区，不表示完全未写入。
 */
int firmware_version_save(flash_if_t *flash, const firmware_version_info_t *info,
                          uint32_t *generation);

/**
 * @brief 从A/B副本加载CRC及字段合法、generation最新的FRU v2记录。
 * @param[in,out] flash 已初始化的同步Flash接口，只要求read回调。
 * @param[out] fru 非NULL输出结构；仅返回FLASH_STORE_OK时有效。
 * @param[out] generation 可为NULL；选中记录的代数，不是输入期望版本。
 * @return FLASH_STORE_OK成功；ERR_PARAM参数无效；ERR_IO无可用副本且访问
 *         失败；ERR_NOT_FOUND两副本空白；ERR_CORRUPT记录损坏；ERR_LEGACY
 *         最新有效记录为已知旧格式；ERR_UNSUPPORTED存在有效未知格式。
 * @note 先逐槽校验再比较代数，不能让较旧v2掩盖较新v1；不写Flash。
 *       仅成功时使用fru，完整事务由调用方串行化。
 */
int fru_info_load(flash_if_t *flash, fru_info_t *fru, uint32_t *generation);

/**
 * @brief 将FRU v2写入较旧或无效副本，并回读验证。
 * @param[in,out] flash 已初始化且提供read/write回调的Flash接口。
 * @param[in] fru 非NULL、schema/size/字段合法的记录，返回前保持有效。
 * @param[out] generation 可为NULL；成功时返回新代数，失败保持原值。
 * @return FLASH_STORE_OK成功；ERR_PARAM参数/记录格式无效；ERR_IO读写失败；
 *         ERR_VERIFY写入后回读校验失败；ERR_UNSUPPORTED有效未知格式禁止覆盖。
 * @note 同步擦写，调用方须串行化完整事务。只更新一个副本并保留另一有效
 *       副本；失败可能已改变目标扇区，不表示完全未写入。
 */
int fru_info_save(flash_if_t *flash, const fru_info_t *fru,
                  uint32_t *generation);

/**
 * @brief 读取旧160/192字节FRU v1并转换成RAM中的v2结构。
 * @param[in,out] flash 已初始化且提供read回调的Flash接口。
 * @param[out] fru 非NULL输出；成功时保留旧身份/网络/保留字段，新字段为空。
 * @param[out] generation 可为NULL；成功时返回选中旧记录的代数。
 * @return FLASH_STORE_OK成功；其他FLASH_STORE_ERR_*区分参数、IO、空白、
 *         损坏或有效未知格式（UNSUPPORTED，禁止覆盖）。
 * @note 不写Flash；调用方补齐新字段后再保存。fru在失败时保持原值。
 */
int fru_info_load_legacy_v1(flash_if_t *flash, fru_info_t *fru,
                            uint32_t *generation);

/**
 * @brief 计算IEEE CRC-32，供持久化记录及离线打包使用。
 * @param[in] data 至少可读length字节，长度为0时允许NULL。
 * @param[in] length 数据字节数。
 * @return CRC值；NULL且长度非0返回0，空数据CRC同样为0。
 * @note 返回值不是独立错误码，调用方仍须保证输入容量正确。
 */
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
