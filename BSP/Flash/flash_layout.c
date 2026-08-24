/*
 * ============================================================================
 * W25Q256 版本信息 / FRU 双副本记录实现
 * ============================================================================
 */
#include "flash_layout.h"

#include <stddef.h>
#include <string.h>

#ifndef FIRMWARE_VERSION_MAJOR
#define FIRMWARE_VERSION_MAJOR 0
#endif
#ifndef FIRMWARE_VERSION_MINOR
#define FIRMWARE_VERSION_MINOR 1
#endif
#ifndef FIRMWARE_VERSION_PATCH
#define FIRMWARE_VERSION_PATCH 0
#endif
#ifndef FIRMWARE_BUILD_NUMBER
#define FIRMWARE_BUILD_NUMBER 0
#endif
#ifndef FIRMWARE_BUILD_TIME_UTC
#define FIRMWARE_BUILD_TIME_UTC "unknown"
#endif
#ifndef FIRMWARE_GIT_COMMIT
#define FIRMWARE_GIT_COMMIT "unknown"
#endif
#ifndef FIRMWARE_BUILD_ID
#define FIRMWARE_BUILD_ID "unknown"
#endif
#ifndef FIRMWARE_TARGET
#define FIRMWARE_TARGET "apollo-v2"
#endif
#ifndef FIRMWARE_GIT_DIRTY
#define FIRMWARE_GIT_DIRTY 0
#endif

/*
 * 当前运行镜像的编译期身份。独立段由链接脚本 KEEP，即使 PC 查询命令尚未
 * 接入，也能在 ELF/量产镜像中定位。长度和 CRC 不能自引用，交由清单填写。
 */
static const firmware_version_info_t s_current_firmware_version
    __attribute__((section(".firmware_version"), used)) = {
    .schema_version = FIRMWARE_VERSION_SCHEMA_VERSION,
    .struct_size    = sizeof(firmware_version_info_t),
    .version_major  = FIRMWARE_VERSION_MAJOR,
    .version_minor  = FIRMWARE_VERSION_MINOR,
    .version_patch  = FIRMWARE_VERSION_PATCH,
    .flags          = FIRMWARE_GIT_DIRTY ? FIRMWARE_VERSION_FLAG_GIT_DIRTY : 0U,
    .build_number   = FIRMWARE_BUILD_NUMBER,
    .image_length   = 0U,
    .image_crc32    = 0U,
    .build_time_utc = FIRMWARE_BUILD_TIME_UTC,
    .git_commit     = FIRMWARE_GIT_COMMIT,
    .build_id       = FIRMWARE_BUILD_ID,
    .target         = FIRMWARE_TARGET,
};

typedef enum {
    SLOT_EMPTY,
    SLOT_VALID,
    SLOT_INVALID,
    SLOT_IO_ERROR,
} slot_state_t;

typedef struct {
    uint32_t slot_a;
    uint32_t slot_b;
    uint16_t record_type;
    uint32_t payload_size;
} record_area_t;

static const record_area_t s_version_area = {
    FLASH_LAYOUT_VERSION_SLOT_A,
    FLASH_LAYOUT_VERSION_SLOT_B,
    FLASH_RECORD_TYPE_FIRMWARE_VERSION,
    sizeof(firmware_version_info_t),
};

static const record_area_t s_fru_area = {
    FLASH_LAYOUT_FRU_SLOT_A,
    FLASH_LAYOUT_FRU_SLOT_B,
    FLASH_RECORD_TYPE_FRU,
    sizeof(fru_info_t),
};

uint32_t flash_crc32(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;

    if (data == NULL && length != 0U) {
        return 0U;
    }

    while (length-- != 0U) {
        crc ^= *data++;
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

const firmware_version_info_t *firmware_version_current(void)
{
    return &s_current_firmware_version;
}

void fru_info_init(fru_info_t *fru)
{
    if (fru == NULL) {
        return;
    }

    memset(fru, 0, sizeof(*fru));
    fru->schema_version = FRU_INFO_SCHEMA_VERSION;
    fru->struct_size = sizeof(*fru);
    memset(fru->reserved, 0xFF, sizeof(fru->reserved));
}

static int flash_interface_is_valid(const flash_if_t *flash)
{
    return flash != NULL && flash->read != NULL && flash->write != NULL;
}

static int fixed_string_is_terminated(const char *text, uint32_t capacity)
{
    return text != NULL && memchr(text, '\0', capacity) != NULL;
}

static int ipv4_is_zero(const uint8_t address[4])
{
    return address[0] == 0U && address[1] == 0U &&
           address[2] == 0U && address[3] == 0U;
}

static int firmware_info_is_valid(const firmware_version_info_t *info)
{
    return info != NULL &&
           info->schema_version == FIRMWARE_VERSION_SCHEMA_VERSION &&
           info->struct_size == sizeof(*info) &&
           fixed_string_is_terminated(info->build_time_utc,
                                      sizeof(info->build_time_utc)) &&
           fixed_string_is_terminated(info->git_commit,
                                      sizeof(info->git_commit)) &&
           fixed_string_is_terminated(info->build_id, sizeof(info->build_id)) &&
           fixed_string_is_terminated(info->target, sizeof(info->target));
}

static int fru_is_valid(const fru_info_t *fru)
{
    return fru != NULL &&
           fru->schema_version == FRU_INFO_SCHEMA_VERSION &&
           fru->struct_size == sizeof(*fru) &&
           fru->mac_valid <= 1U &&
           fru->network_mode <= FRU_NETWORK_MODE_DHCP &&
           fixed_string_is_terminated(fru->product_name,
                                      sizeof(fru->product_name)) &&
           fixed_string_is_terminated(fru->part_number,
                                      sizeof(fru->part_number)) &&
           fixed_string_is_terminated(fru->serial_number,
                                      sizeof(fru->serial_number)) &&
           fixed_string_is_terminated(fru->hardware_revision,
                                      sizeof(fru->hardware_revision)) &&
           fixed_string_is_terminated(fru->manufacturing_date,
                                      sizeof(fru->manufacturing_date)) &&
           fixed_string_is_terminated(fru->site_label,
                                      sizeof(fru->site_label)) &&
           (fru->network_mode == FRU_NETWORK_MODE_UNSET ||
            fru->service_port != 0U) &&
           (fru->network_mode != FRU_NETWORK_MODE_STATIC ||
            (!ipv4_is_zero(fru->configured_ipv4) &&
             !ipv4_is_zero(fru->configured_netmask)));
}

static int generation_is_newer(uint32_t left, uint32_t right)
{
    /* 有符号差值使 0xFFFFFFFF -> 0 的自然回绕仍能判定为“更新”。 */
    return (int32_t)(left - right) > 0;
}

static slot_state_t read_slot(flash_if_t *flash, const record_area_t *area,
                              uint32_t address, void *payload,
                              flash_record_header_t *header)
{
    flash_record_header_t local_header;
    uint32_t expected_header_crc;

    if (flash->read(flash, address, (uint8_t *)&local_header,
                    sizeof(local_header)) != IF_OK) {
        return SLOT_IO_ERROR;
    }

    if (local_header.magic == 0xFFFFFFFFUL) {
        return SLOT_EMPTY;
    }

    expected_header_crc = flash_crc32((const uint8_t *)&local_header,
                                      offsetof(flash_record_header_t, header_crc32));
    if (local_header.magic != FLASH_RECORD_MAGIC ||
        local_header.format_version != FLASH_RECORD_FORMAT_VERSION ||
        local_header.record_type != area->record_type ||
        local_header.payload_length != area->payload_size ||
        local_header.header_crc32 != expected_header_crc) {
        return SLOT_INVALID;
    }

    if (flash->read(flash, address + sizeof(local_header), payload,
                    area->payload_size) != IF_OK) {
        return SLOT_IO_ERROR;
    }
    if (flash_crc32(payload, area->payload_size) != local_header.payload_crc32) {
        return SLOT_INVALID;
    }

    if (header != NULL) {
        *header = local_header;
    }
    return SLOT_VALID;
}

static int load_record(flash_if_t *flash, const record_area_t *area,
                       void *payload, uint32_t *generation)
{
    uint8_t payload_a[sizeof(fru_info_t)];
    uint8_t payload_b[sizeof(fru_info_t)];
    flash_record_header_t header_a;
    flash_record_header_t header_b;
    slot_state_t state_a;
    slot_state_t state_b;
    const void *selected_payload;
    uint32_t selected_generation;

    if (!flash_interface_is_valid(flash) || area == NULL || payload == NULL ||
        area->payload_size > sizeof(payload_a)) {
        return FLASH_STORE_ERR_PARAM;
    }

    state_a = read_slot(flash, area, area->slot_a, payload_a, &header_a);
    state_b = read_slot(flash, area, area->slot_b, payload_b, &header_b);

    if (state_a == SLOT_VALID && state_b == SLOT_VALID) {
        if (generation_is_newer(header_b.generation, header_a.generation)) {
            selected_payload = payload_b;
            selected_generation = header_b.generation;
        } else {
            selected_payload = payload_a;
            selected_generation = header_a.generation;
        }
    } else if (state_a == SLOT_VALID) {
        selected_payload = payload_a;
        selected_generation = header_a.generation;
    } else if (state_b == SLOT_VALID) {
        selected_payload = payload_b;
        selected_generation = header_b.generation;
    } else {
        if (state_a == SLOT_IO_ERROR || state_b == SLOT_IO_ERROR) {
            return FLASH_STORE_ERR_IO;
        }
        if (state_a == SLOT_INVALID || state_b == SLOT_INVALID) {
            return FLASH_STORE_ERR_CORRUPT;
        }
        return FLASH_STORE_ERR_NOT_FOUND;
    }

    memcpy(payload, selected_payload, area->payload_size);
    if (generation != NULL) {
        *generation = selected_generation;
    }
    return FLASH_STORE_OK;
}

static int save_record(flash_if_t *flash, const record_area_t *area,
                       const void *payload, uint32_t *generation)
{
    uint8_t probe_payload[sizeof(fru_info_t)];
    uint8_t record[sizeof(flash_record_header_t) + sizeof(fru_info_t)];
    flash_record_header_t header_a;
    flash_record_header_t header_b;
    flash_record_header_t new_header;
    flash_record_header_t verify_header;
    slot_state_t state_a;
    slot_state_t state_b;
    uint32_t target_address;
    uint32_t next_generation;

    if (!flash_interface_is_valid(flash) || area == NULL || payload == NULL ||
        area->payload_size > sizeof(probe_payload)) {
        return FLASH_STORE_ERR_PARAM;
    }

    state_a = read_slot(flash, area, area->slot_a, probe_payload, &header_a);
    state_b = read_slot(flash, area, area->slot_b, probe_payload, &header_b);
    if (state_a == SLOT_IO_ERROR || state_b == SLOT_IO_ERROR) {
        return FLASH_STORE_ERR_IO;
    }

    if (state_a == SLOT_VALID && state_b == SLOT_VALID) {
        if (generation_is_newer(header_b.generation, header_a.generation)) {
            target_address = area->slot_a;
            next_generation = header_b.generation + 1U;
        } else {
            target_address = area->slot_b;
            next_generation = header_a.generation + 1U;
        }
    } else if (state_a == SLOT_VALID) {
        target_address = area->slot_b;
        next_generation = header_a.generation + 1U;
    } else if (state_b == SLOT_VALID) {
        target_address = area->slot_a;
        next_generation = header_b.generation + 1U;
    } else {
        target_address = area->slot_a;
        next_generation = 1U;
    }

    memset(&new_header, 0xFF, sizeof(new_header));
    new_header.magic = FLASH_RECORD_MAGIC;
    new_header.format_version = FLASH_RECORD_FORMAT_VERSION;
    new_header.record_type = area->record_type;
    new_header.generation = next_generation;
    new_header.payload_length = area->payload_size;
    new_header.payload_crc32 = flash_crc32(payload, area->payload_size);
    new_header.header_crc32 = flash_crc32((const uint8_t *)&new_header,
                                         offsetof(flash_record_header_t, header_crc32));

    memcpy(record, &new_header, sizeof(new_header));
    memcpy(record + sizeof(new_header), payload, area->payload_size);

    /*
     * flash_if.write 对目标扇区执行一次完整 RMW。这里只发起一次连续写，
     * 避免“先写头、再写载荷”造成同一扇区被擦写两次。
     */
    if (flash->write(flash, target_address, record,
                     sizeof(new_header) + area->payload_size) != IF_OK) {
        return FLASH_STORE_ERR_IO;
    }

    if (read_slot(flash, area, target_address, probe_payload, &verify_header) != SLOT_VALID ||
        verify_header.generation != next_generation ||
        memcmp(probe_payload, payload, area->payload_size) != 0) {
        return FLASH_STORE_ERR_VERIFY;
    }

    if (generation != NULL) {
        *generation = next_generation;
    }
    return FLASH_STORE_OK;
}

int firmware_version_load(flash_if_t *flash, firmware_version_info_t *info,
                          uint32_t *generation)
{
    int result = load_record(flash, &s_version_area, info, generation);

    if (result == FLASH_STORE_OK && !firmware_info_is_valid(info)) {
        return FLASH_STORE_ERR_CORRUPT;
    }
    return result;
}

int firmware_version_save(flash_if_t *flash, const firmware_version_info_t *info,
                          uint32_t *generation)
{
    if (!firmware_info_is_valid(info)) {
        return FLASH_STORE_ERR_PARAM;
    }
    return save_record(flash, &s_version_area, info, generation);
}

int fru_info_load(flash_if_t *flash, fru_info_t *fru, uint32_t *generation)
{
    int result = load_record(flash, &s_fru_area, fru, generation);

    if (result == FLASH_STORE_OK && !fru_is_valid(fru)) {
        return FLASH_STORE_ERR_CORRUPT;
    }
    return result;
}

int fru_info_save(flash_if_t *flash, const fru_info_t *fru,
                  uint32_t *generation)
{
    if (!fru_is_valid(fru)) {
        return FLASH_STORE_ERR_PARAM;
    }
    return save_record(flash, &s_fru_area, fru, generation);
}
