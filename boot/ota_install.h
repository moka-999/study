#ifndef OTA_INSTALL_H
#define OTA_INSTALL_H

#include <stdint.h>

/* 包格式v1固定使用A1基准镜像；外部文件不允许指定内部擦写地址。 */
#define OTA_A1_BASE             UINT32_C(0x08020000)
#define OTA_A2_BASE             UINT32_C(0x08100000)
#define OTA_IMAGE_LIMIT         UINT32_C(0x000E0000)
#define OTA_DOWNLOAD_BASE       UINT32_C(0x00010000)
#define OTA_STABLE_BASE         UINT32_C(0x00210000)
#define OTA_PACKAGE_LIMIT       UINT32_C(0x00200000)
#define OTA_SECTOR_BYTES        UINT32_C(0x00020000)
#define OTA_FLASH_WORD_BYTES    32U
#define OTA_HEADER_BYTES        40U
#define OTA_VERSION_BYTES       76U
#define OTA_PREFIX_BYTES        (OTA_HEADER_BYTES + OTA_VERSION_BYTES)

/** 返回原因供启动管理记录；任何非OTA_OK结果都不得授权目标槽启动。 */
typedef enum {
    OTA_OK = 0,
    OTA_ERR_ARGUMENT,
    OTA_ERR_FORMAT,
    OTA_ERR_COMPATIBILITY,
    OTA_ERR_BOARD,
    OTA_ERR_VERSION,
    OTA_ERR_RANGE,
    OTA_ERR_CRC,
    OTA_ERR_RELOCATION,
    OTA_ERR_VECTOR,
    OTA_ERR_READ,
    OTA_ERR_STATE,
    OTA_ERR_ERASE,
    OTA_ERR_PROGRAM,
    OTA_ERR_VERIFY
} ota_status_t;

/**
 * 可信启动记录提供的普通升级请求，不得直接使用PC传入的“当前槽/当前版本”。
 * confirmed_base必须指向已确认可用的App；试运行期间禁止覆盖回退槽。
 * package_bytes和package_header_crc32绑定已经提交的下载文件，防止串用旧请求。
 * recovery只能由B的稳定版恢复阶段置1：读取固定稳定区，目标固定A1，允许降版。
 * 该字段不在升级文件/PC报文中；普通下载路径必须保持0。
 */
typedef struct {
    uint32_t confirmed_base;
    uint32_t package_bytes;
    uint32_t package_header_crc32;
    uint16_t current_version[3];
    char target[16];                 /**< 本机板型，非空，末尾及剩余字节为零。 */
    uint32_t recovery;               /**< 0普通升级，1受控稳定版恢复，其他值拒绝。 */
} ota_request_t;

/** 安装结果仅描述镜像，不表示已提交TRIAL或健康确认；失败时整个输出清零。 */
typedef struct {
    uint32_t slot_base;
    uint32_t image_bytes;
    uint32_t image_crc32;
    uint32_t package_header_crc32;
    uint8_t version_payload[OTA_VERSION_BYTES]; /**< 既有76B格式，CRC已改为目标槽CRC。 */
} ota_image_t;

/**
 * 安装核心与硬件/持久化层的唯一边界，离线测试注入同一组操作。
 * 所有操作同步完成，0表示成功，非0表示失败；必须自带有限超时。
 * 安装期间源包与请求必须保持不变，实际端口按有效进展处理看门狗。
 * erase/program端口还须二次检查白名单；H743同bank擦写的完整关键路径放RAM。
 * read_internal遇到不可纠正ECC必须报错，不得返回未确认的数据。
 * 完整B连接真实端口，OTA版App只复用只读校验；离线测试替换IO，不替换本核心。
 */
typedef struct {
    void *context;
    int (*read_package)(void *context, uint32_t address, uint8_t *data, uint32_t bytes);
    /** 必须先可靠保存INSTALLING/目标无效；没有启动记录实现时不能假返回成功。 */
    int (*mark_installing)(void *context, const ota_image_t *image);
    int (*erase_sector)(void *context, uint32_t address);
    /** address和data均32B对齐，每次只写一个32B Flash-word。 */
    int (*program_word)(void *context, uint32_t address, const uint8_t *data);
    int (*read_internal)(void *context, uint32_t address, uint8_t *data, uint32_t bytes);
} ota_io_t;

/**
 * @brief 与zlib及现有flash_crc32一致的IEEE CRC32，支持分块续算。
 * @param crc 首块传0，后续传上次返回值；data在bytes非零时必须有效。
 * @return 已完成最终异或的CRC；零长度不读取data，原样返回crc。
 */
uint32_t ota_crc32(uint32_t crc, const uint8_t *data, uint32_t bytes);

/**
 * @brief 只读完整校验包头、板型、版本、修正表、两种镜像CRC及向量。
 * @param io 只要求read_package有效，读取仅限固定下载区。
 * @param request 可信请求；out不得与输入对象重叠。
 * @param out 成功输出目标描述，失败清零；不会擦写任何存储。
 */
ota_status_t ota_package_validate(const ota_io_t *io, const ota_request_t *request,
                                  ota_image_t *out);

/** 仅检查前116字节的格式、长度、板型、版本和包头CRC；不能据此安装或提交READY。 */
ota_status_t ota_package_validate_prefix(const ota_io_t *io, const ota_request_t *request);

/**
 * @brief 重新完整校验，再标记目标无效、擦除非活动槽、修正/写入、内部读回校验。
 * @param io 五个操作都必须有效；request/out要求同上，整个调用阻塞且不分配堆。
 * @return OTA_OK仅表示安装读回通过；调用方仍须可靠提交TRIAL并同步版本后才可跳转。
 * @note 失败不重试、不切槽、不更新FRU/版本记录；调用方保留INSTALLING状态。
 *       掉电后应从可信记录恢复同一请求/同一目标重新安装，不能反转来源和目标。
 */
ota_status_t ota_install(const ota_io_t *io, const ota_request_t *request, ota_image_t *out);

/** 启动前重新核对已安装镜像的范围、向量、CRC；不能以记录有效代替实际Flash校验。 */
ota_status_t ota_installed_validate(const ota_io_t *io, const ota_image_t *image);

#endif
