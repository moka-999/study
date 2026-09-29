/* App只下载、验证、提交请求及确认健康；内部Flash安装和选槽由B完成。 */
#include "ota_app.h"
#include "boot_record.h"
#include "boot_health.h"
#include "flash_bsp.h"
#include "flash_layout.h"
#include "qspi_hw.h"
#include "monitor_task.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include <string.h>

static boot_health_t health;
/* 通用健康观察器的槽位容量可大于实际任务数，禁止漏检或访问越界。 */
_Static_assert(OTA_TASK_COUNT <= BOOT_HEALTH_TASKS, "OTA任务数超过健康观察器容量");
static uint32_t boot_epoch, running_base;
static volatile uint32_t confirmed, failed, download_state;
static uint32_t download_bytes, downloaded, download_id;
static uint32_t last_offset, last_bytes, last_progress_tick;
static volatile uint32_t commit_tick;
static ota_request_t download_request;
static int initialized;

/* 可由SWD读取当前健康阶段；地址以本次App ELF为准，不能沿用旧地址。 */
volatile uint32_t ota_app_diag[8];

static int read_store(void *ctx, uint32_t address, uint8_t *data, uint32_t bytes)
{
    (void)ctx;
    if (address >= 0x6000U || bytes > 0x6000U - address) return -1;
    return qspi_read_data(address, data, bytes);
}

static int program_store(void *ctx, uint32_t address, const uint8_t *data, uint32_t bytes)
{
    (void)ctx;
    if (address < BOOT_JOURNAL_BASE || address >= 0x6000U || bytes > 0x6000U - address) return -1;
    while (bytes != 0U) {
        uint32_t n = 256U - (address & 255U); if (n > bytes) n = bytes;
        if (qspi_write_page(address, data, (uint16_t)n) != 0) return -1;
        address += n; data += n; bytes -= n;
    }
    return 0;
}

static int erase_store(void *ctx, uint32_t address)
{
    (void)ctx;
    if (address != BOOT_JOURNAL_BASE && address != BOOT_JOURNAL_BASE + 4096U) return -1;
    return qspi_erase_sector(address);
}

static const boot_store_t store = {NULL, read_store, program_store, erase_store};

void ota_app_prepare(void)
{
    boot_health_init(&health, (1U << OTA_TASK_COUNT) - 1U, HAL_GetTick());
    running_base = SCB->VTOR;
    ota_app_diag[0] = 0x4F544131U; ota_app_diag[1] = running_base;
}

void ota_app_progress(uint32_t task_id)
{
    if (task_id >= OTA_TASK_COUNT) return;
    taskENTER_CRITICAL();
    boot_health_progress(&health, task_id, HAL_GetTick());
    taskEXIT_CRITICAL();
}

int ota_app_watchdog_tick(void)
{
    /* 独立于TCP发送成功与否，提交后最多4秒+一次500ms调度周期交给B裁决。 */
    if ((download_state == OTA_DOWNLOAD_READY || download_state == OTA_DOWNLOAD_COMMIT_UNKNOWN) &&
        (uint32_t)(HAL_GetTick() - commit_tick) >= 4000U) {
        NVIC_SystemReset();
        return 0;
    }
    if (failed != 0U) return 0;
    if (!initialized) {
        boot_record_t record;
        if (flash_transaction_begin(1000U) != 0) { failed = 1U; return 0; }
        int result = boot_record_load(&store, &record);
        flash_transaction_end();
        if (result != 0 || boot_slot_base(record.active) != running_base || record.pending != 1U ||
            (record.phase != BOOT_NORMAL && record.phase != BOOT_TRIAL)) {
            failed = 2U; ota_app_diag[2] = failed; return 0;
        }
        boot_epoch = record.epoch; initialized = 1;
        ota_app_diag[3] = boot_epoch;
    }
    taskENTER_CRITICAL();
    int status = boot_health_poll(&health, HAL_GetTick());
    ota_app_diag[4] = health.seen_mask;
    taskEXIT_CRITICAL();
    if (status == 0) { failed = 3U; ota_app_diag[2] = failed; return 0; }
    if (status == 2 && confirmed == 0U) {
        boot_record_t record;
        if (flash_transaction_begin(1000U) != 0) { failed = 4U; return 0; }
        int result = boot_record_load(&store, &record);
        if (result == 0) result = boot_record_confirm(&record, running_base, boot_epoch);
        if (result == 0) result = boot_record_save(&store, &record);
        flash_transaction_end();
        if (result != 0) { failed = 5U; ota_app_diag[2] = failed; return 0; }
        confirmed = 1U; ota_app_diag[5] = HAL_GetTick();
    }
    return 1;
}

int ota_app_is_confirmed(void)
{
    return confirmed != 0U && failed == 0U &&
           download_state != OTA_DOWNLOAD_RECEIVING && download_state != OTA_DOWNLOAD_READY &&
           download_state != OTA_DOWNLOAD_COMMIT_UNKNOWN;
}

/** 快照必须新鲜且明确为OFF；本轮不猜测尚未闭环的RK安全关机是否已完成。 */
static int safe_idle(void)
{
    monitor_snapshot_t snapshot;
    return monitor_get_latest(&snapshot) && snapshot.power_state == PWR_CTRL_STATE_OFF &&
           (uint32_t)(HAL_GetTick() - snapshot.timestamp_ms) <= 5000U &&
           pwr_ctrl_get_state() == PWR_CTRL_STATE_OFF;
}

static int read_download(void *ctx, uint32_t address, uint8_t *data, uint32_t bytes)
{
    (void)ctx;
    if (address < OTA_DOWNLOAD_BASE || address - OTA_DOWNLOAD_BASE > downloaded ||
        bytes > downloaded - (address - OTA_DOWNLOAD_BASE)) return -1;
    int result = qspi_read_data(address, data, bytes);
    if (result == 0) ota_app_progress(OTA_TASK_TCP);
    return result;
}

static const ota_io_t download_reader = {.read_package = read_download};

/** 仅清理尚未提交的会话；保留失败/取消状态供当前连接查询，不擦除下载数据。 */
static void end_receiving(ota_download_state_t state)
{
    if (download_state == OTA_DOWNLOAD_RECEIVING) monitor_ota_leave();
    download_state = state; ota_app_diag[6] = state;
}

static ota_download_result_t validation_result(ota_status_t status)
{
    if (status == OTA_OK) return OTA_DOWNLOAD_OK;
    if (status == OTA_ERR_BOARD) return OTA_DOWNLOAD_TARGET;
    if (status == OTA_ERR_VERSION) return OTA_DOWNLOAD_VERSION;
    if (status == OTA_ERR_CRC) return OTA_DOWNLOAD_CRC;
    if (status == OTA_ERR_READ) return OTA_DOWNLOAD_FLASH;
    return OTA_DOWNLOAD_PACKAGE;
}

/** 调用者持有Flash事务锁。运行槽、版本记录与启动日志必须一致，不能猜测。 */
static int load_running(boot_record_t *record, firmware_version_info_t *version)
{
    if (boot_record_load(&store, record) != 0 ||
        firmware_version_load(&flash_if, version, NULL) != FLASH_STORE_OK ||
        boot_slot_base(record->active) != running_base ||
        (record->valid_mask & (1U << record->active)) == 0U ||
        (initialized && record->epoch != boot_epoch) ||
        memcmp(version, record->images[record->active].version_payload, OTA_VERSION_BYTES) != 0) return -1;
    return 0;
}

int ota_app_load_status(ota_app_status_t *out)
{
    boot_record_t record;
    firmware_version_info_t version;
    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    if (flash_transaction_begin(1000U) != 0) return -1;
    int result = load_running(&record, &version);
    if (result == 0) {
        out->active_package_id = record.images[record.active].package_header_crc32;
        out->version[0] = version.version_major;
        out->version[1] = version.version_minor;
        out->version[2] = version.version_patch;
        out->confirmed = confirmed != 0U && failed == 0U && record.pending == 0U &&
                         (record.phase == BOOT_NORMAL || record.phase == BOOT_READY);
    }
    flash_transaction_end();
    return result;
}

void ota_download_get_progress(ota_download_progress_t *out)
{
    out->state = (ota_download_state_t)download_state;
    out->package_id = download_id;
    out->total_bytes = download_bytes;
    out->received_bytes = downloaded;
}

void ota_download_poll(void)
{
    if (download_state == OTA_DOWNLOAD_RECEIVING &&
        (uint32_t)(HAL_GetTick() - last_progress_tick) >= 30000U) end_receiving(OTA_DOWNLOAD_CANCELLED);
}

void ota_download_disconnected(void)
{
    if (download_state == OTA_DOWNLOAD_READY || download_state == OTA_DOWNLOAD_COMMIT_UNKNOWN) return;
    end_receiving(OTA_DOWNLOAD_IDLE);
    download_id = download_bytes = downloaded = 0U;
}

ota_download_result_t ota_download_begin(uint32_t package_id, uint32_t bytes)
{
    boot_record_t record;
    firmware_version_info_t version;
    if (bytes < OTA_PREFIX_BYTES || bytes > OTA_PACKAGE_LIMIT) return OTA_DOWNLOAD_ARGUMENT;
    ota_download_poll();
    if (download_state == OTA_DOWNLOAD_RECEIVING) {
        if (package_id != download_id) return OTA_DOWNLOAD_BUSY;
        return bytes == download_bytes ? OTA_DOWNLOAD_OK : OTA_DOWNLOAD_ARGUMENT;
    }
    if (download_state == OTA_DOWNLOAD_READY || download_state == OTA_DOWNLOAD_COMMIT_UNKNOWN) return OTA_DOWNLOAD_STATE;
    if (!ota_app_is_confirmed()) return OTA_DOWNLOAD_NOT_READY;
    if (!safe_idle()) return OTA_DOWNLOAD_STATE;
    if (monitor_ota_enter() != 0) return OTA_DOWNLOAD_BUSY;
    if (flash_transaction_begin(1000U) != 0) { monitor_ota_leave(); return OTA_DOWNLOAD_FLASH; }
    int result = load_running(&record, &version);
    flash_transaction_end();
    if (result != 0 || record.phase != BOOT_NORMAL || record.pending != 0U) {
        monitor_ota_leave(); return OTA_DOWNLOAD_FLASH;
    }
    memset(&download_request, 0, sizeof(download_request));
    download_request.confirmed_base = running_base;
    download_request.package_bytes = bytes;
    download_request.package_header_crc32 = package_id;
    memcpy(download_request.current_version, record.images[record.active].version_payload + 4, 6U);
    memcpy(download_request.target, version.target, sizeof(download_request.target));
    download_id = package_id; download_bytes = bytes; downloaded = 0U;
    last_offset = UINT32_MAX; last_bytes = 0U; last_progress_tick = HAL_GetTick();
    download_state = OTA_DOWNLOAD_RECEIVING;
    ota_app_diag[6] = download_state; ota_app_diag[7] = 0U;
    return OTA_DOWNLOAD_OK;
}

ota_download_result_t ota_download_write(uint32_t package_id, uint32_t offset,
                                         const uint8_t *data, uint32_t bytes)
{
    uint8_t verify[256];
    ota_download_result_t result = OTA_DOWNLOAD_OK;
    ota_download_poll();
    if (download_state != OTA_DOWNLOAD_RECEIVING || package_id != download_id || !safe_idle()) return OTA_DOWNLOAD_STATE;
    if (data == NULL || bytes == 0U || bytes > OTA_DOWNLOAD_BLOCK_BYTES || offset >= download_bytes || bytes > download_bytes - offset)
        return OTA_DOWNLOAD_ARGUMENT;
    int duplicate = offset == last_offset;
    if (duplicate && bytes != last_bytes) return OTA_DOWNLOAD_DATA;
    if (!duplicate && offset != downloaded) return OTA_DOWNLOAD_OFFSET;
    if (flash_transaction_begin(1000U) != 0) { end_receiving(OTA_DOWNLOAD_FAILED); return OTA_DOWNLOAD_FLASH; }
    uint32_t cursor = 0U;
    while (cursor < bytes) {
        uint32_t address = OTA_DOWNLOAD_BASE + offset + cursor;
        uint32_t n = 256U - (address & 255U); if (n > bytes - cursor) n = bytes - cursor;
        /* 重试块只读比较；新块按需擦扇区，禁止失败后对半写页继续编程。 */
        if (!duplicate && (((address & 4095U) == 0U && qspi_erase_sector(address) != 0) ||
            qspi_write_page(address, data + cursor, (uint16_t)n) != 0)) { result = OTA_DOWNLOAD_FLASH; break; }
        if (qspi_read_data(address, verify, n) != 0) { result = OTA_DOWNLOAD_FLASH; break; }
        if (memcmp(verify, data + cursor, n) != 0) {
            result = duplicate ? OTA_DOWNLOAD_DATA : OTA_DOWNLOAD_FLASH; break;
        }
        cursor += n; ota_app_progress(OTA_TASK_TCP);
    }
    if (result == OTA_DOWNLOAD_OK && !duplicate) {
        uint32_t before = downloaded;
        downloaded += bytes;
        if (before < OTA_PREFIX_BYTES && downloaded >= OTA_PREFIX_BYTES)
            result = validation_result(ota_package_validate_prefix(&download_reader, &download_request));
    }
    flash_transaction_end();
    if (result == OTA_DOWNLOAD_OK) {
        last_offset = offset; last_bytes = bytes; last_progress_tick = HAL_GetTick();
        ota_app_diag[7] = downloaded;
    } else if (result != OTA_DOWNLOAD_DATA) end_receiving(OTA_DOWNLOAD_FAILED);
    return result;
}

ota_download_result_t ota_download_finish(uint32_t package_id)
{
    boot_record_t record;
    firmware_version_info_t version;
    ota_image_t image;
    ota_download_poll();
    if (package_id != download_id) return OTA_DOWNLOAD_STATE;
    if (download_state == OTA_DOWNLOAD_READY) return OTA_DOWNLOAD_OK;
    if (download_state == OTA_DOWNLOAD_COMMIT_UNKNOWN) return OTA_DOWNLOAD_COMMIT_ERROR;
    if (download_state != OTA_DOWNLOAD_RECEIVING || downloaded != download_bytes || !safe_idle()) return OTA_DOWNLOAD_STATE;
    if (flash_transaction_begin(1000U) != 0) { end_receiving(OTA_DOWNLOAD_FAILED); return OTA_DOWNLOAD_FLASH; }
    ota_download_result_t result = validation_result(ota_package_validate(&download_reader, &download_request, &image));
    if (result == OTA_DOWNLOAD_OK && (load_running(&record, &version) != 0 ||
        record.phase != BOOT_NORMAL || record.pending != 0U)) result = OTA_DOWNLOAD_FLASH;
    if (result == OTA_DOWNLOAD_OK && !safe_idle()) result = OTA_DOWNLOAD_STATE;
    int committing = result == OTA_DOWNLOAD_OK;
    if (committing) {
        record.phase = BOOT_READY; record.package_bytes = download_bytes;
        record.package_crc = download_id; record.install_tries = 0U;
        if (boot_record_save(&store, &record) != 0) result = OTA_DOWNLOAD_COMMIT_ERROR;
    }
    flash_transaction_end();
    if (committing) {
        /* 先发布时间再发布状态；看门狗任务只能在完整提交尝试结束后开始复位计时。 */
        commit_tick = HAL_GetTick();
        download_state = result == OTA_DOWNLOAD_OK ? OTA_DOWNLOAD_READY : OTA_DOWNLOAD_COMMIT_UNKNOWN;
        ota_app_diag[6] = download_state;
    } else end_receiving(OTA_DOWNLOAD_FAILED);
    return result;
}

ota_download_result_t ota_download_cancel(uint32_t package_id)
{
    if (package_id != download_id) return OTA_DOWNLOAD_STATE;
    if (download_state == OTA_DOWNLOAD_CANCELLED) return OTA_DOWNLOAD_OK;
    if (download_state != OTA_DOWNLOAD_RECEIVING) return OTA_DOWNLOAD_STATE;
    end_receiving(OTA_DOWNLOAD_CANCELLED);
    return OTA_DOWNLOAD_OK;
}
