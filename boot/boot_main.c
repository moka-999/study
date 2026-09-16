#include "boot_port.h"
#include <string.h>

/* 板型在B构建时固定，不能由下载包覆盖；两板禁止交叉安装。 */
#if defined(BOOT_BOARD_B1)
static const char board_target[16] = "b1-h743";
#else
static const char board_target[16] = "apollo-v2-h743";
#endif
static boot_record_t state;
static const boot_store_t store = {NULL, boot_nor_read, boot_nor_program, boot_nor_erase};

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}
static void put32(uint8_t *p, uint32_t value)
{
    for (uint32_t i = 0; i < 4U; ++i) p[i] = (uint8_t)(value >> (8U * i));
}

static void save_or_stop(void)
{
    if (boot_record_save(&store, &state) != 0) boot_maintenance(0x1001U);
    boot_diag[1] = state.phase; boot_diag[5] = state.generation;
}

/** 核心要求的安装屏障：目标无效状态持久化成功后，才打开硬件擦写白名单。 */
static int mark_installing(void *context, const ota_image_t *image)
{
    (void)context;
    uint32_t destination = image->slot_base == OTA_A1_BASE ? 0U : 1U;
    if (state.phase != BOOT_INSTALLING || (state.stage < 2U && destination == state.active)) return -1;
    state.valid_mask &= ~(1U << destination);
    if (state.previous == destination) state.previous = BOOT_SLOT_NONE;
    if (boot_record_save(&store, &state) != 0) return -1;
    boot_allow_target(image->slot_base);
    return 0;
}

static const ota_io_t installer = {NULL, boot_nor_read, mark_installing,
    boot_flash_erase, boot_flash_program, boot_flash_read};

/**
 * 兼容既有B1RC版本记录：保持一个有效副本，仅写另一4KiB扇区。
 * 每次跳转前同步实际槽的76B信息；相同内容不写。FRU的0x2000..0x3FFF完全不触碰。
 */
static int sync_version(const ota_image_t *im)
{
    uint8_t records[2][108], output[108], verify[108];
    int good[2] = {0, 0};
    for (uint32_t i = 0; i < 2U; ++i) {
        uint8_t *r = records[i];
        if (boot_nor_read(NULL, i * 4096U, r, 108U) != 0) return -1;
        good[i] = get32(r) == 0x43523142U && get32(r + 4) == 0x00010001U &&
            get32(r + 12) == OTA_VERSION_BYTES && get32(r + 20) == ota_crc32(0U, r, 20U) &&
            get32(r + 16) == ota_crc32(0U, r + 32, OTA_VERSION_BYTES);
    }
    int latest = good[0] ? 0 : good[1] ? 1 : -1;
    if (good[0] && good[1]) {
        uint32_t delta = get32(records[1] + 8) - get32(records[0] + 8);
        if (delta == 0x80000000U) return -1;
        if (delta != 0U && delta < 0x80000000U) latest = 1;
    }
    if (latest >= 0 && memcmp(records[latest] + 32, im->version_payload, OTA_VERSION_BYTES) == 0) return 0;
    uint32_t destination = latest == 0 ? 4096U : 0U;
    memset(output, 255, sizeof(output));
    put32(output, 0x43523142U); put32(output + 4, 0x00010001U);
    put32(output + 8, latest >= 0 ? get32(records[latest] + 8) + 1U : 1U);
    put32(output + 12, OTA_VERSION_BYTES);
    memcpy(output + 32, im->version_payload, OTA_VERSION_BYTES);
    put32(output + 16, ota_crc32(0U, output + 32, OTA_VERSION_BYTES));
    put32(output + 20, ota_crc32(0U, output, 20U));
    if (boot_nor_erase(NULL, destination) != 0 ||
        boot_nor_program(NULL, destination + 32U, output + 32, OTA_VERSION_BYTES) != 0 ||
        boot_nor_program(NULL, destination, output, 32U) != 0 ||
        boot_nor_read(NULL, destination, verify, sizeof(verify)) != 0) return -1;
    return memcmp(output, verify, sizeof(output)) == 0 ? 0 : -1;
}

/** 普通安装失败回到未被覆盖的当前版本；稳定源失败留在维护，不反复擦除。 */
static void reject_install(uint32_t error)
{
    boot_allow_target(0U); state.last_error = error;
    state.package_bytes = 0U; state.package_crc = 0U;
    if (state.stage == 2U) { state.phase = BOOT_MAINTENANCE; state.stage = 3U; }
    else { state.phase = BOOT_NORMAL; state.install_tries = 0U; }
    save_or_stop();
}

static void install_pending(void)
{
    ota_request_t request = {0};
    ota_image_t image;
    request.confirmed_base = boot_slot_base(state.active);
    memcpy(request.target, board_target, sizeof(board_target));
    for (uint32_t i = 0; i < 3U; ++i) {
        const uint8_t *v = state.images[state.active].version_payload + 4U + 2U * i;
        request.current_version[i] = (uint16_t)((uint16_t)v[0] | (uint16_t)((uint16_t)v[1] << 8U));
    }
    request.recovery = state.stage == 2U ? 1U : 0U;
    if (request.recovery != 0U) {
        uint8_t header[OTA_HEADER_BYTES];
        if (boot_nor_read(NULL, OTA_STABLE_BASE, header, sizeof(header)) != 0) {
            reject_install(OTA_ERR_READ); return;
        }
        state.package_bytes = get32(header + 12); state.package_crc = get32(header + 36);
        if (state.package_bytes < OTA_PREFIX_BYTES || state.package_bytes > OTA_PACKAGE_LIMIT) {
            state.package_bytes = 0U; reject_install(OTA_ERR_RANGE); return;
        }
    }
    request.package_bytes = state.package_bytes; request.package_header_crc32 = state.package_crc;
    if (state.install_tries >= 3U) { reject_install(OTA_ERR_STATE); return; }
    /* 在耗时动作前计入安装尝试；每次复位仍从该请求的同一目标重新开始。 */
    ++state.install_tries; state.phase = BOOT_INSTALLING; save_or_stop();
    ota_status_t result = ota_install(&installer, &request, &image);
    boot_allow_target(0U);
    if (result != OTA_OK) { reject_install((uint32_t)result); return; }
    uint32_t target = image.slot_base == OTA_A1_BASE ? 0U : 1U;
    if (request.recovery != 0U) {
        state.previous = BOOT_SLOT_NONE; state.valid_mask = 1U;
    } else {
        state.previous = state.active; state.valid_mask |= 1U << target;
    }
    state.active = target; state.images[target] = image;
    state.phase = BOOT_TRIAL; state.pending = 0U; state.failures = 0U;
    state.install_tries = 0U; state.package_bytes = 0U; state.package_crc = 0U;
    save_or_stop();
}

void boot_main(void)
{
    if (boot_port_init() != 0) boot_maintenance(0x1000U);
    /* 首次部署由维护工具据精确App镜像创建初始记录；全空白也不猜测有效App。 */
    int result = boot_record_load(&store, &state);
    if (result != BOOT_STORE_OK) boot_maintenance(result == BOOT_STORE_EMPTY ? 0x1002U : 0x1003U);
    if (state.pending != 0U) { boot_record_failed_start(&state); save_or_stop(); }
    for (uint32_t attempt = 0U; attempt < 4U; ++attempt) {
        if (state.phase == BOOT_MAINTENANCE) boot_maintenance(state.last_error);
        if (state.phase == BOOT_READY || state.phase == BOOT_INSTALLING) install_pending();
        if (state.phase == BOOT_MAINTENANCE) boot_maintenance(state.last_error);
        const ota_image_t *im = &state.images[state.active];
        if ((state.valid_mask & (1U << state.active)) == 0U ||
            memcmp(im->version_payload + 60, board_target, sizeof(board_target)) != 0 ||
            ota_installed_validate(&installer, im) != OTA_OK) {
            state.last_error = OTA_ERR_VERIFY; boot_record_fallback(&state); save_or_stop(); continue;
        }
        if (sync_version(im) != 0) boot_maintenance(0x1004U);
        /* 必须先落盘，再跳转；epoch拒绝上一启动会话遗留的健康确认。 */
        state.pending = 1U; ++state.epoch; save_or_stop();
        boot_jump(im->slot_base);
    }
    boot_maintenance(0x1005U);
}
