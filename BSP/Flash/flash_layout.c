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
#ifndef FIRMWARE_BUILD_TIME_UTC
#define FIRMWARE_BUILD_TIME_UTC "unknown"
#endif
#ifndef FIRMWARE_BUILD_ID
#define FIRMWARE_BUILD_ID "unknown"
#endif
#ifndef FIRMWARE_TARGET
#define FIRMWARE_TARGET "apollo-v2"
#endif
/*
 * 当前运行镜像的编译期身份。独立段由链接脚本 KEEP，即使 PC 查询命令尚未
 * 接入，也能在ELF中定位。长度和CRC保留为0；本结构不会自动写入W25。
 */
static const firmware_version_info_t s_current_firmware_version
    __attribute__((section(".firmware_version"), used)) = {
    .schema_version = FIRMWARE_VERSION_SCHEMA_VERSION,
    .struct_size    = sizeof(firmware_version_info_t),
    .version_major  = FIRMWARE_VERSION_MAJOR,
    .version_minor  = FIRMWARE_VERSION_MINOR,
    .version_patch  = FIRMWARE_VERSION_PATCH,
    .image_length   = 0U,
    .image_crc32    = 0U,
    .build_time_utc = FIRMWARE_BUILD_TIME_UTC,
    .build_id       = FIRMWARE_BUILD_ID,
    .target         = FIRMWARE_TARGET,
};

typedef enum {
    SLOT_EMPTY,
    SLOT_VALID,
    SLOT_INVALID,
    SLOT_IO_ERROR,
    SLOT_UNSUPPORTED,
} slot_state_t;

typedef struct {
    uint32_t slot_a;
    uint32_t slot_b;
    uint16_t record_type;
    uint32_t payload_size;
} record_area_t;

/*
 * FRU v1只用于从已量产/已调试的Flash平滑迁移。它与当前v2的
 * 前138字节字段顺序相同，但在site_label后直接进入22字节保留区。
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
    uint8_t  reserved[22];
} fru_info_v1_t;

_Static_assert(sizeof(fru_info_v1_t) == 160U, "legacy FRU v1 layout changed");
_Static_assert(offsetof(fru_info_v1_t, site_label) == 122U,
               "legacy FRU v1 field offset changed");

/* B1早期布局含料号和MAC有效位，不能按V2的160字节v1解释。 */
typedef struct {
    uint16_t schema_version;
    uint16_t struct_size;
    char product_name[32];
    char part_number[32];
    char serial_number[32];
    char hardware_revision[16];
    char manufacturing_date[16];
    uint8_t mac_address[6];
    uint8_t mac_valid;
    uint8_t network_mode;
    uint8_t configured_ipv4[4];
    uint8_t configured_netmask[4];
    uint8_t configured_gateway[4];
    uint16_t service_port;
    char site_label[16];
    uint8_t reserved[22];
} fru_info_b1_v1_t;

/* 旧版本信息只读兼容；flags/build_number/git_commit不进入精简后的输出。 */
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
    char build_time_utc[24];
    char git_commit[16];
    char build_id[16];
    char target[16];
} firmware_version_legacy_t;

_Static_assert(sizeof(fru_info_b1_v1_t) == 192U, "B1 legacy FRU size changed");
_Static_assert(offsetof(fru_info_b1_v1_t, serial_number) == 68U,
               "B1 legacy serial number offset changed");
_Static_assert(sizeof(firmware_version_legacy_t) == 96U,
               "legacy firmware version size changed");

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

static const record_area_t s_fru_v1_area = {
    FLASH_LAYOUT_FRU_SLOT_A,
    FLASH_LAYOUT_FRU_SLOT_B,
    FLASH_RECORD_TYPE_FRU,
    sizeof(fru_info_v1_t),
};

static const record_area_t s_fru_b1_v1_area = {
    FLASH_LAYOUT_FRU_SLOT_A, FLASH_LAYOUT_FRU_SLOT_B,
    FLASH_RECORD_TYPE_FRU, sizeof(fru_info_b1_v1_t),
};

static const record_area_t s_version_legacy_area = {
    FLASH_LAYOUT_VERSION_SLOT_A, FLASH_LAYOUT_VERSION_SLOT_B,
    FLASH_RECORD_TYPE_FIRMWARE_VERSION, sizeof(firmware_version_legacy_t),
};

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t length)
{
    while (length-- != 0U) {
        crc ^= *data++;
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
        }
    }
    return crc;
}

uint32_t flash_crc32(const uint8_t *data, uint32_t length)
{
    if (data == NULL && length != 0U) return 0U;
    return crc32_update(0xFFFFFFFFUL, data, length) ^ 0xFFFFFFFFUL;
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
    return flash != NULL && flash->read != NULL;
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
           fixed_string_is_terminated(info->build_id, sizeof(info->build_id)) &&
           fixed_string_is_terminated(info->target, sizeof(info->target));
}

static int fru_is_valid(const fru_info_t *fru)
{
    return fru != NULL &&
           fru->schema_version == FRU_INFO_SCHEMA_VERSION &&
           fru->struct_size == sizeof(*fru) &&
           fru->network_mode <= FRU_NETWORK_MODE_DHCP &&
           fixed_string_is_terminated(fru->product_name,
                                      sizeof(fru->product_name)) &&
           fixed_string_is_terminated(fru->serial_number,
                                      sizeof(fru->serial_number)) &&
           fixed_string_is_terminated(fru->hardware_revision,
                                      sizeof(fru->hardware_revision)) &&
           fixed_string_is_terminated(fru->manufacturing_date,
                                      sizeof(fru->manufacturing_date)) &&
           fixed_string_is_terminated(fru->site_label,
                                      sizeof(fru->site_label)) &&
           fixed_string_is_terminated(fru->tester_name,
                                      sizeof(fru->tester_name)) &&
           fixed_string_is_terminated(fru->test_board,
                                      sizeof(fru->test_board)) &&
           (fru->network_mode == FRU_NETWORK_MODE_UNSET ||
            fru->service_port != 0U) &&
           (fru->network_mode != FRU_NETWORK_MODE_STATIC ||
            (!ipv4_is_zero(fru->configured_ipv4) &&
             !ipv4_is_zero(fru->configured_netmask)));
}

static int fru_v1_is_valid(const fru_info_v1_t *fru)
{
    return fru != NULL &&
           fru->schema_version == 1U &&
           fru->struct_size == sizeof(*fru) &&
           fru->network_mode <= FRU_NETWORK_MODE_DHCP &&
           fixed_string_is_terminated(fru->product_name,
                                      sizeof(fru->product_name)) &&
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

static int legacy_version_is_valid(const firmware_version_legacy_t *info)
{
    return info->schema_version == 1U && info->struct_size == sizeof(*info) &&
           fixed_string_is_terminated(info->build_time_utc, sizeof(info->build_time_utc)) &&
           fixed_string_is_terminated(info->git_commit, sizeof(info->git_commit)) &&
           fixed_string_is_terminated(info->build_id, sizeof(info->build_id)) &&
           fixed_string_is_terminated(info->target, sizeof(info->target));
}

static int fru_b1_v1_is_valid(const fru_info_b1_v1_t *fru)
{
    return fru->schema_version == 1U && fru->struct_size == sizeof(*fru) &&
           fru->mac_valid <= 1U && fru->network_mode <= FRU_NETWORK_MODE_DHCP &&
           fixed_string_is_terminated(fru->product_name, sizeof(fru->product_name)) &&
           fixed_string_is_terminated(fru->part_number, sizeof(fru->part_number)) &&
           fixed_string_is_terminated(fru->serial_number, sizeof(fru->serial_number)) &&
           fixed_string_is_terminated(fru->hardware_revision, sizeof(fru->hardware_revision)) &&
           fixed_string_is_terminated(fru->manufacturing_date, sizeof(fru->manufacturing_date)) &&
           fixed_string_is_terminated(fru->site_label, sizeof(fru->site_label)) &&
           (fru->network_mode == FRU_NETWORK_MODE_UNSET || fru->service_port != 0U) &&
           (fru->network_mode != FRU_NETWORK_MODE_STATIC ||
            (!ipv4_is_zero(fru->configured_ipv4) && !ipv4_is_zero(fru->configured_netmask)));
}

/*
 * CRC只证明字节未损坏，不证明字符串/网络字段合法。每槽在成为SLOT_VALID
 * 前都要检查语义，不能选出latest后才发现错误并丢弃另一个可用副本。
 * 调用者可能传uint8_t数组；先拷入对齐union，禁止把字节缓冲强转为结构指针。
 */
static int record_payload_is_valid(const record_area_t *area, const void *payload)
{
    union {
        fru_info_t fru;
        fru_info_v1_t fru160;
        fru_info_b1_v1_t fru192;
        firmware_version_info_t version;
        firmware_version_legacy_t version96;
    } aligned;
    if (area->payload_size > sizeof(aligned)) return 0;
    memcpy(&aligned, payload, area->payload_size);
    if (area->record_type == FLASH_RECORD_TYPE_FRU) {
        if (area->payload_size == sizeof(aligned.fru)) return fru_is_valid(&aligned.fru);
        if (area->payload_size == sizeof(aligned.fru160)) return fru_v1_is_valid(&aligned.fru160);
        if (area->payload_size == sizeof(aligned.fru192)) return fru_b1_v1_is_valid(&aligned.fru192);
    } else if (area->record_type == FLASH_RECORD_TYPE_FIRMWARE_VERSION) {
        if (area->payload_size == sizeof(aligned.version)) return firmware_info_is_valid(&aligned.version);
        if (area->payload_size == sizeof(aligned.version96)) return legacy_version_is_valid(&aligned.version96);
    }
    return 0;
}

/* 只识别已知持久化ABI；同为schema=1也必须同时核对struct_size。 */
static int record_layout_is_known(const flash_record_header_t *header,
                                  uint16_t expected_type, const uint16_t prefix[2])
{
    if (header->format_version != FLASH_RECORD_FORMAT_VERSION ||
        header->record_type != expected_type || prefix[1] != header->payload_length) {
        return 0;
    }
    if (expected_type == FLASH_RECORD_TYPE_FRU) {
        return (prefix[0] == 2U && prefix[1] == sizeof(fru_info_t)) ||
               (prefix[0] == 1U && (prefix[1] == sizeof(fru_info_v1_t) ||
                                    prefix[1] == sizeof(fru_info_b1_v1_t)));
    }
    return prefix[0] == 1U && (prefix[1] == sizeof(firmware_version_info_t) ||
                              prefix[1] == sizeof(firmware_version_legacy_t));
}

/* 未知格式不能借用当前结构大小读取；分块验证CRC，最多读取本4KiB扇区。 */
static slot_state_t check_unknown_record(flash_if_t *flash, uint32_t address,
                                         const flash_record_header_t *header)
{
    uint8_t chunk[64];
    uint32_t crc = 0xFFFFFFFFUL;
    if (header->payload_length > FLASH_LAYOUT_SECTOR_SIZE - sizeof(*header)) {
        /* 头CRC有效却超出当前槽容量，保守拒绝覆盖，也不能越界探测。 */
        return SLOT_UNSUPPORTED;
    }
    for (uint32_t offset = 0U; offset < header->payload_length;) {
        uint32_t count = header->payload_length - offset;
        if (count > sizeof(chunk)) count = sizeof(chunk);
        if (flash->read(flash, address + sizeof(*header) + offset, chunk, count) != IF_OK) {
            return SLOT_IO_ERROR;
        }
        crc = crc32_update(crc, chunk, count);
        offset += count;
    }
    return (crc ^ 0xFFFFFFFFUL) == header->payload_crc32
               ? SLOT_UNSUPPORTED : SLOT_INVALID;
}

static slot_state_t read_slot(flash_if_t *flash, const record_area_t *area,
                              uint32_t address, void *payload,
                              flash_record_header_t *header)
{
    flash_record_header_t local_header;
    uint32_t expected_header_crc;
    uint16_t prefix[2] = {0U, 0U};

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
        local_header.header_crc32 != expected_header_crc) {
        return SLOT_INVALID;
    }

    if (local_header.payload_length >= sizeof(prefix) &&
        local_header.payload_length <= FLASH_LAYOUT_SECTOR_SIZE - sizeof(local_header)) {
        if (flash->read(flash, address + sizeof(local_header),
                        (uint8_t *)prefix, sizeof(prefix)) != IF_OK) return SLOT_IO_ERROR;
    }
    if (!record_layout_is_known(&local_header, area->record_type, prefix)) {
        return check_unknown_record(flash, address, &local_header);
    }
    /* 已知旧格式交给相应兼容入口；不同大小不能写入当前调用者的缓冲。 */
    if (local_header.payload_length != area->payload_size) return SLOT_INVALID;

    if (flash->read(flash, address + sizeof(local_header), payload,
                    area->payload_size) != IF_OK) {
        return SLOT_IO_ERROR;
    }
    if (flash_crc32(payload, area->payload_size) != local_header.payload_crc32) {
        return SLOT_INVALID;
    }
    if (!record_payload_is_valid(area, payload)) return SLOT_INVALID;

    if (header != NULL) {
        *header = local_header;
    }
    return SLOT_VALID;
}

/* 读取并转换一个旧FRU副本；这里只操作RAM，不擦写旧记录。 */
static slot_state_t read_legacy_fru_slot(flash_if_t *flash, uint32_t address,
                                         fru_info_t *fru, flash_record_header_t *header)
{
    union { fru_info_v1_t v1; fru_info_b1_v1_t b1; } legacy;
    slot_state_t state = read_slot(flash, &s_fru_v1_area, address, &legacy.v1, header);
    fru_info_init(fru);
    if (state == SLOT_VALID) {
        /* 160字节v1与v2的公共字段前缀相同，跳过schema/size以保留v2标识。 */
        memcpy((uint8_t *)fru + 4U, (const uint8_t *)&legacy.v1 + 4U,
               offsetof(fru_info_t, tester_name) - 4U);
        memcpy(fru->reserved, legacy.v1.reserved, sizeof(fru->reserved));
        return SLOT_VALID;
    }
    if (state != SLOT_INVALID) return state;
    state = read_slot(flash, &s_fru_b1_v1_area, address, &legacy.b1, header);
    if (state != SLOT_VALID) return state;
    memcpy(fru->product_name, legacy.b1.product_name, sizeof(fru->product_name));
    memcpy(fru->serial_number, legacy.b1.serial_number, sizeof(fru->serial_number));
    memcpy(fru->hardware_revision, legacy.b1.hardware_revision, sizeof(fru->hardware_revision));
    memcpy(fru->manufacturing_date, legacy.b1.manufacturing_date, sizeof(fru->manufacturing_date));
    if (legacy.b1.mac_valid) memcpy(fru->mac_address, legacy.b1.mac_address, sizeof(fru->mac_address));
    fru->network_mode = legacy.b1.network_mode;
    memcpy(fru->configured_ipv4, legacy.b1.configured_ipv4, sizeof(fru->configured_ipv4));
    memcpy(fru->configured_netmask, legacy.b1.configured_netmask, sizeof(fru->configured_netmask));
    memcpy(fru->configured_gateway, legacy.b1.configured_gateway, sizeof(fru->configured_gateway));
    fru->service_port = legacy.b1.service_port;
    memcpy(fru->site_label, legacy.b1.site_label, sizeof(fru->site_label));
    memcpy(fru->reserved, legacy.b1.reserved, sizeof(fru->reserved));
    return fru_is_valid(fru) ? SLOT_VALID : SLOT_INVALID;
}

/* 写入新格式前也把旧副本纳入代次比较，永远先替换较旧副本。 */
static slot_state_t read_legacy_slot(flash_if_t *flash, const record_area_t *area,
                                     uint32_t address, flash_record_header_t *header)
{
    if (area->record_type == FLASH_RECORD_TYPE_FRU) {
        fru_info_t fru;
        return read_legacy_fru_slot(flash, address, &fru, header);
    }
    firmware_version_legacy_t version;
    return read_slot(flash, &s_version_legacy_area, address, &version, header);
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

    /* 即使另一个副本可读，也不能把有效未知格式当损坏并触发默认写入。 */
    if (state_a == SLOT_UNSUPPORTED || state_b == SLOT_UNSUPPORTED) {
        return FLASH_STORE_ERR_UNSUPPORTED;
    }

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

    if (!flash_interface_is_valid(flash) || flash->write == NULL || area == NULL || payload == NULL ||
        area->payload_size > sizeof(probe_payload)) {
        return FLASH_STORE_ERR_PARAM;
    }

    state_a = read_slot(flash, area, area->slot_a, probe_payload, &header_a);
    state_b = read_slot(flash, area, area->slot_b, probe_payload, &header_b);
    if (state_a == SLOT_INVALID) state_a = read_legacy_slot(flash, area, area->slot_a, &header_a);
    if (state_b == SLOT_INVALID) state_b = read_legacy_slot(flash, area, area->slot_b, &header_b);
    if (state_a == SLOT_UNSUPPORTED || state_b == SLOT_UNSUPPORTED) {
        return FLASH_STORE_ERR_UNSUPPORTED;
    }
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
    firmware_version_legacy_t legacy;
    uint32_t current_generation = 0U;
    uint32_t legacy_generation = 0U;
    if (info == NULL) return FLASH_STORE_ERR_PARAM;
    int result = load_record(flash, &s_version_area, info, &current_generation);
    if (result == FLASH_STORE_ERR_UNSUPPORTED || result == FLASH_STORE_ERR_PARAM) return result;
    int old_result = load_record(flash, &s_version_legacy_area, &legacy, &legacy_generation);
    if (old_result == FLASH_STORE_ERR_UNSUPPORTED) return old_result;
    if (old_result == FLASH_STORE_OK && (result != FLASH_STORE_OK ||
        generation_is_newer(legacy_generation, current_generation))) {
        /* 只读转换：响应数据仍来自W25，不回退到编译期身份，也不写旧扇区。 */
        memset(info, 0, sizeof(*info));
        info->schema_version = FIRMWARE_VERSION_SCHEMA_VERSION;
        info->struct_size = sizeof(*info);
        info->version_major = legacy.version_major;
        info->version_minor = legacy.version_minor;
        info->version_patch = legacy.version_patch;
        info->image_length = legacy.image_length;
        info->image_crc32 = legacy.image_crc32;
        memcpy(info->build_time_utc, legacy.build_time_utc, sizeof(info->build_time_utc));
        memcpy(info->build_id, legacy.build_id, sizeof(info->build_id));
        memcpy(info->target, legacy.target, sizeof(info->target));
        current_generation = legacy_generation;
        result = FLASH_STORE_OK;
    }
    if (result == FLASH_STORE_OK && generation != NULL) *generation = current_generation;
    if (result != FLASH_STORE_OK && old_result == FLASH_STORE_ERR_IO) return old_result;
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
    fru_info_t legacy;
    uint32_t current_generation = 0U;
    uint32_t legacy_generation = 0U;
    int result = load_record(flash, &s_fru_area, fru, &current_generation);
    if (result == FLASH_STORE_ERR_PARAM || result == FLASH_STORE_ERR_UNSUPPORTED) return result;
    int old_result = fru_info_load_legacy_v1(flash, &legacy, &legacy_generation);
    if (old_result == FLASH_STORE_ERR_UNSUPPORTED) return old_result;
    if (old_result == FLASH_STORE_OK && (result != FLASH_STORE_OK ||
        generation_is_newer(legacy_generation, current_generation))) {
        /* 较新的旧格式不能被较旧v2掩盖；由查询入口完成显式迁移，绝不默认重建。 */
        memset(fru, 0, sizeof(*fru));
        if (generation != NULL) *generation = legacy_generation;
        return FLASH_STORE_ERR_LEGACY;
    }
    if (result == FLASH_STORE_OK && generation != NULL) *generation = current_generation;
    if (result != FLASH_STORE_OK && old_result == FLASH_STORE_ERR_IO) return old_result;
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

int fru_info_load_legacy_v1(flash_if_t *flash, fru_info_t *fru,
                            uint32_t *generation)
{
    fru_info_t a, b;
    flash_record_header_t header_a, header_b;
    if (!flash_interface_is_valid(flash) || fru == NULL) return FLASH_STORE_ERR_PARAM;
    slot_state_t state_a = read_legacy_fru_slot(flash, FLASH_LAYOUT_FRU_SLOT_A, &a, &header_a);
    slot_state_t state_b = read_legacy_fru_slot(flash, FLASH_LAYOUT_FRU_SLOT_B, &b, &header_b);
    if (state_a == SLOT_UNSUPPORTED || state_b == SLOT_UNSUPPORTED) return FLASH_STORE_ERR_UNSUPPORTED;
    if (state_a != SLOT_VALID && state_b != SLOT_VALID) {
        if (state_a == SLOT_IO_ERROR || state_b == SLOT_IO_ERROR) return FLASH_STORE_ERR_IO;
        return state_a == SLOT_INVALID || state_b == SLOT_INVALID
                   ? FLASH_STORE_ERR_CORRUPT : FLASH_STORE_ERR_NOT_FOUND;
    }
    int use_b = state_b == SLOT_VALID && (state_a != SLOT_VALID ||
                 generation_is_newer(header_b.generation, header_a.generation));
    *fru = use_b ? b : a;
    if (generation != NULL) *generation = use_b ? header_b.generation : header_a.generation;
    return FLASH_STORE_OK;
}
