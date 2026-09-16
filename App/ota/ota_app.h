#ifndef OTA_APP_H
#define OTA_APP_H
#include <stdint.h>

enum { OTA_TASK_DEFAULT = 0, OTA_TASK_TCP, OTA_TASK_MONITOR, OTA_TASK_RK, OTA_TASK_HB };

#if defined(ENABLE_OTA_BOOT) && ENABLE_OTA_BOOT
/** 调度前初始化健康观察器；失败记录不会被默认健康掩盖。 */
void ota_app_prepare(void);
/** 每个必要任务在完成一轮工作/有界等待后上报；不是外设在线状态。 */
void ota_app_progress(uint32_t task_id);
/** 仅由看门狗任务每500ms调用；返回1才允许喂狗，健康失效永久锁存至复位。 */
int ota_app_watchdog_tick(void);
/** 确认前拒绝测试/电源变更/下一次升级；查询和心跳不受影响。 */
int ota_app_is_confirmed(void);

/** 下载状态仅由TCP任务修改；READY/提交不确定在复位前始终锁住OFF维护门。 */
typedef enum {
    OTA_DOWNLOAD_IDLE = 0, OTA_DOWNLOAD_RECEIVING, OTA_DOWNLOAD_READY,
    OTA_DOWNLOAD_COMMIT_UNKNOWN, OTA_DOWNLOAD_CANCELLED, OTA_DOWNLOAD_FAILED
} ota_download_state_t;

/** 不携带PC命令号；协议层将这些原因映射为JSON错误码。 */
typedef enum {
    OTA_DOWNLOAD_OK = 0, OTA_DOWNLOAD_ARGUMENT, OTA_DOWNLOAD_STATE, OTA_DOWNLOAD_BUSY,
    OTA_DOWNLOAD_NOT_READY, OTA_DOWNLOAD_OFFSET, OTA_DOWNLOAD_DATA,
    OTA_DOWNLOAD_PACKAGE, OTA_DOWNLOAD_TARGET, OTA_DOWNLOAD_VERSION,
    OTA_DOWNLOAD_CRC, OTA_DOWNLOAD_FLASH, OTA_DOWNLOAD_COMMIT_ERROR
} ota_download_result_t;

typedef struct {
    ota_download_state_t state;
    uint32_t package_id;       /**< 包头CRC32，0也是有效ID，不能作为无包标记。 */
    uint32_t total_bytes;      /**< 完整.ota文件长度，不是其中App镜像长度。 */
    uint32_t received_bytes;   /**< 已写入并读回确认的连续前缀。 */
} ota_download_progress_t;

typedef struct {
    uint32_t active_package_id; /**< 从启动记录加载的当前槽包ID。 */
    uint16_t version[3];        /**< 每次从W25版本记录加载，并与当前槽描述交叉核对。 */
    uint8_t confirmed;          /**< 当前App已健康确认；下载中仍可为1。 */
} ota_app_status_t;

/**
 * 所有下载接口由唯一TCP任务顺序调用。begin同ID/长度幂等；write只允许顺序块
 * 或紧邻上一成功块的原样重试。解码后每块1..1024字节，不保存整个包到RAM。
 * finish完整校验并提交READY；无论应答送达与否，看门狗任务在提交后约4秒复位。
 */
ota_download_result_t ota_download_begin(uint32_t package_id, uint32_t package_bytes);
ota_download_result_t ota_download_write(uint32_t package_id, uint32_t offset,
                                         const uint8_t *data, uint32_t bytes);
ota_download_result_t ota_download_finish(uint32_t package_id);
ota_download_result_t ota_download_cancel(uint32_t package_id);
/** 周期调用处理30秒接收空闲；query/心跳/begin重试不会续期。 */
void ota_download_poll(void);
/** 断线取消未提交事务；READY/提交不确定必须保留，等待B重新裁决。 */
void ota_download_disconnected(void);
/** 复制RAM进度，不访问存储、不刷新超时。out须非NULL。 */
void ota_download_get_progress(ota_download_progress_t *out);
/** 只读持久化版本/启动记录；0成功，非0为读失败或数据相互矛盾，不填默认值。 */
int ota_app_load_status(ota_app_status_t *out);
#else
static inline void ota_app_prepare(void) { }
static inline void ota_app_progress(uint32_t task_id) { (void)task_id; }
static inline int ota_app_watchdog_tick(void) { return 1; }
static inline int ota_app_is_confirmed(void) { return 1; }
#endif
#endif
