#include "ota_install.h"

#include <stddef.h>
#include <string.h>

#define OTA_MAGIC UINT32_C(0x3141544F) /* 小端字节为OTA1。 */
#define OTA_FORMAT_VERSION 1U
#define OTA_BOOT_API_VERSION 1U
#define OTA_CHUNK_BYTES 256U

/** 解码后的内部描述；线上字段逐字节读取，不依赖C结构体填充/对齐。 */
typedef struct {
    ota_image_t image;
    uint32_t relocation_count;
    uint32_t table_crc32;
    uint32_t source_crc32;
    uint32_t a2_crc32;
    uint32_t image_offset;
} package_t;

/** 允许非对齐字段与跨Flash-word的修正位置，不进行未对齐uint32_t解引用。 */
static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8U));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static void write_u32(uint8_t *p, uint32_t value)
{
    for (uint32_t i = 0U; i < 4U; ++i) {
        p[i] = (uint8_t)(value >> (8U * i));
    }
}

uint32_t ota_crc32(uint32_t crc, const uint8_t *data, uint32_t bytes)
{
    /* 沿用App flash_layout的逐位算法；入口/出口异或使接口能直接续算。 */
    crc ^= UINT32_C(0xFFFFFFFF);
    while (bytes-- != 0U) {
        crc ^= *data++;
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & (0U - (crc & 1U)));
        }
    }
    return crc ^ UINT32_C(0xFFFFFFFF);
}

/** 固定长度C字符串必须非空、存在终止符，终止符之后全零，禁止越界strcmp。 */
static int valid_text(const uint8_t *text, uint32_t size)
{
    uint32_t i = 0U;
    if (text[0] == 0U) return 0;
    while (i < size && text[i] != 0U) ++i;
    if (i == size) return 0;
    while (i < size) {
        if (text[i++] != 0U) return 0;
    }
    return 1;
}

/** 只允许项目实际使用的数据RAM窗口；栈向下增长，允许顶端但排除底端。 */
static int valid_vectors(const uint8_t *data, uint32_t base, uint32_t bytes)
{
    uint32_t sp = read_u32(data);
    uint32_t entry = read_u32(data + 4U);
    int ram_ok = (sp > UINT32_C(0x20000000) && sp <= UINT32_C(0x20020000)) ||
                 (sp > UINT32_C(0x24000000) && sp <= UINT32_C(0x24080000)) ||
                 (sp > UINT32_C(0x30000000) && sp <= UINT32_C(0x30048000)) ||
                 (sp > UINT32_C(0x38000000) && sp <= UINT32_C(0x38010000));
    return ram_ok && (sp & 7U) == 0U && (entry & 1U) != 0U &&
           (entry & ~UINT32_C(1)) >= base + 8U &&
           (entry & ~UINT32_C(1)) <= base + bytes - 2U;
}

/** 所有外部读取统一加白名单保护；减法比较避免溢出后绕过检查。 */
static ota_status_t read_source(const ota_io_t *io, const ota_request_t *request,
                                uint32_t offset, uint8_t *data, uint32_t bytes)
{
    if (offset > request->package_bytes || bytes > request->package_bytes - offset) {
        return OTA_ERR_RANGE;
    }
    uint32_t base = request->recovery == 1U ? OTA_STABLE_BASE : OTA_DOWNLOAD_BASE;
    return io->read_package(io->context, base + offset, data, bytes) == 0
           ? OTA_OK : OTA_ERR_READ;
}

/** 包头与已有76B版本载荷解析；此处尚未验证整个镜像，不能据此擦除。 */
static ota_status_t read_package(const ota_io_t *io, const ota_request_t *request,
                                 package_t *package)
{
    uint8_t prefix[OTA_PREFIX_BYTES];
    const uint8_t *version = prefix + OTA_HEADER_BYTES;
    ota_status_t status;
    int newer = 0;

    if (request->recovery > 1U ||
        (request->confirmed_base != OTA_A1_BASE && request->confirmed_base != OTA_A2_BASE)) {
        return OTA_ERR_ARGUMENT;
    }
    if (!valid_text((const uint8_t *)request->target, sizeof(request->target))) {
        return OTA_ERR_ARGUMENT;
    }
    if (request->package_bytes < OTA_PREFIX_BYTES || request->package_bytes > OTA_PACKAGE_LIMIT) {
        return OTA_ERR_RANGE;
    }
    status = read_source(io, request, 0U, prefix, sizeof(prefix));
    if (status != OTA_OK) return status;
    if (read_u32(prefix) != OTA_MAGIC || read_u16(prefix + 4U) != OTA_FORMAT_VERSION ||
        read_u16(prefix + 6U) != OTA_HEADER_BYTES || read_u16(prefix + 10U) != 0U ||
        read_u32(prefix + 32U) != 0U) return OTA_ERR_FORMAT;
    if (read_u16(prefix + 8U) == 0U || read_u16(prefix + 8U) > OTA_BOOT_API_VERSION) {
        return OTA_ERR_COMPATIBILITY;
    }
    if (ota_crc32(0U, prefix, 36U) != read_u32(prefix + 36U) ||
        read_u32(prefix + 36U) != request->package_header_crc32 ||
        ota_crc32(0U, version, OTA_VERSION_BYTES) != read_u32(prefix + 28U)) {
        return OTA_ERR_CRC;
    }
    if (read_u32(prefix + 12U) != request->package_bytes) return OTA_ERR_RANGE;
    if (read_u16(version) != 1U || read_u16(version + 2U) != OTA_VERSION_BYTES ||
        read_u16(version + 10U) != 0U || !valid_text(version + 20U, 24U) ||
        !valid_text(version + 44U, 16U) || !valid_text(version + 60U, 16U)) {
        return OTA_ERR_FORMAT;
    }
    if (memcmp(version + 60U, request->target, 16U) != 0) return OTA_ERR_BOARD;
    for (uint32_t i = 0U; i < 3U; ++i) {
        uint16_t proposed = read_u16(version + 4U + 2U * i);
        if (proposed != request->current_version[i]) {
            newer = proposed > request->current_version[i];
            break;
        }
    }
    if (!newer && request->recovery == 0U) return OTA_ERR_VERSION;

    package->image.slot_base = request->recovery == 1U ? OTA_A1_BASE :
                              request->confirmed_base == OTA_A1_BASE ? OTA_A2_BASE : OTA_A1_BASE;
    package->image.image_bytes = read_u32(version + 12U);
    package->source_crc32 = read_u32(version + 16U);
    package->relocation_count = read_u32(prefix + 16U);
    package->table_crc32 = read_u32(prefix + 20U);
    package->a2_crc32 = read_u32(prefix + 24U);
    if (package->image.image_bytes < 8U || package->image.image_bytes > OTA_IMAGE_LIMIT ||
        package->relocation_count == 0U ||
        package->relocation_count > (package->image.image_bytes - 4U) / 4U) {
        return OTA_ERR_RANGE;
    }
    package->image_offset = OTA_PREFIX_BYTES + 4U * package->relocation_count;
    if (package->image_offset > request->package_bytes ||
        package->image.image_bytes != request->package_bytes - package->image_offset) {
        return OTA_ERR_RANGE;
    }
    package->image.image_crc32 = package->image.slot_base == OTA_A1_BASE
                              ? package->source_crc32 : package->a2_crc32;
    package->image.package_header_crc32 = read_u32(prefix + 36U);
    memcpy(package->image.version_payload, version, OTA_VERSION_BYTES);
    write_u32(package->image.version_payload + 16U, package->image.image_crc32);
    return OTA_OK;
}

/** 顺序读一个修正偏移，同时检查排序、重叠、范围并累计表CRC。 */
static ota_status_t next_relocation(const ota_io_t *io, const ota_request_t *request,
                                    const package_t *package, uint32_t index,
                                    uint32_t *offset, uint32_t *table_crc)
{
    uint8_t value[4];
    uint32_t previous = *offset;
    ota_status_t status;
    if (index == package->relocation_count) {
        *offset = UINT32_MAX;
        return OTA_OK;
    }
    status = read_source(io, request, OTA_PREFIX_BYTES + 4U * index, value, sizeof(value));
    if (status != OTA_OK) return status;
    *offset = read_u32(value);
    if ((index == 0U && *offset != 4U) ||
        (index != 0U && *offset < previous + 4U) ||
        *offset > package->image.image_bytes - 4U) return OTA_ERR_RELOCATION;
    *table_crc = ota_crc32(*table_crc, value, sizeof(value));
    return OTA_OK;
}

/** 统一32B编程入口；只对最后不满一字的数据补FF，不污染修正用的前瞻字节。 */
static ota_status_t program_chunk(const ota_io_t *io, uint32_t address,
                                  const uint8_t *data, uint32_t bytes)
{
    _Alignas(OTA_FLASH_WORD_BYTES) uint8_t tail[OTA_FLASH_WORD_BYTES];
    for (uint32_t j = 0U; j < bytes; j += OTA_FLASH_WORD_BYTES) {
        const uint8_t *word = data + j;
        if (bytes - j < sizeof(tail)) {
            memset(tail, 0xFF, sizeof(tail));
            memcpy(tail, word, bytes - j);
            word = tail;
        }
        if (io->program_word(io->context, address + j, word) != 0) return OTA_ERR_PROGRAM;
    }
    return OTA_OK;
}

/**
 * 两次复用同一遍历：首遍只读校验，第二遍才写入已标无效的目标槽。
 * 每次保留额外3字节处理跨256B边界的非对齐32位地址，修正不会丢掉尾字节。
 * 不把全包或全表搬到RAM；源CRC在修正前累计，A2 CRC在修正后累计。
 */
static ota_status_t walk_image(const ota_io_t *io, const ota_request_t *request,
                               const package_t *package, int program)
{
    _Alignas(OTA_FLASH_WORD_BYTES) uint8_t data[OTA_CHUNK_BYTES + OTA_FLASH_WORD_BYTES];
    uint8_t carry[3];
    uint32_t carry_bytes = 0U, next = 0U, index = 0U;
    uint32_t raw_crc = 0U, a2_crc = 0U, table_crc = 0U;
    uint32_t image_bytes = package->image.image_bytes;
    ota_status_t status = next_relocation(io, request, package, index, &next, &table_crc);
    if (status != OTA_OK) return status;

    for (uint32_t position = 0U; position < image_bytes; position += OTA_CHUNK_BYTES) {
        uint32_t count = image_bytes - position;
        uint32_t extra;
        if (count > OTA_CHUNK_BYTES) count = OTA_CHUNK_BYTES;
        extra = image_bytes - position - count;
        if (extra > sizeof(carry)) extra = sizeof(carry);
        status = read_source(io, request, package->image_offset + position, data, count + extra);
        if (status != OTA_OK) return status;
        raw_crc = ota_crc32(raw_crc, data, count);
        if (position == 0U && !valid_vectors(data, OTA_A1_BASE, image_bytes)) return OTA_ERR_VECTOR;

        /* A1安装先写原始字节；所有修正后的数据只用于另一路A2校验。 */
        if (program && package->image.slot_base == OTA_A1_BASE) {
            status = program_chunk(io, OTA_A1_BASE + position, data, count);
            if (status != OTA_OK) return status;
        }
        memcpy(data, carry, carry_bytes);
        while (next < position + count) {
            uint32_t relative = next - position;
            uint32_t value = read_u32(data + relative);
            /* 与ELF提取器一致，允许一过尾符号；RAM/外设地址不能列入表。 */
            if (value < OTA_A1_BASE || value > OTA_A1_BASE + image_bytes) return OTA_ERR_RELOCATION;
            write_u32(data + relative, value + OTA_A2_BASE - OTA_A1_BASE);
            status = next_relocation(io, request, package, ++index, &next, &table_crc);
            if (status != OTA_OK) return status;
        }
        if (position == 0U && !valid_vectors(data, OTA_A2_BASE, image_bytes)) return OTA_ERR_VECTOR;
        a2_crc = ota_crc32(a2_crc, data, count);
        memcpy(carry, data + count, extra);
        carry_bytes = extra;
        if (program && package->image.slot_base == OTA_A2_BASE) {
            status = program_chunk(io, OTA_A2_BASE + position, data, count);
            if (status != OTA_OK) return status;
        }
    }
    if (index != package->relocation_count) return OTA_ERR_RELOCATION;
    if (raw_crc != package->source_crc32 || a2_crc != package->a2_crc32 ||
        table_crc != package->table_crc32) return OTA_ERR_CRC;
    return OTA_OK;
}

/** 所有公开入口失败时清空输出；先检查指针再读取请求。 */
static ota_status_t validate(const ota_io_t *io, const ota_request_t *request,
                             package_t *package)
{
    ota_status_t status;
    if (io == NULL || request == NULL || io->read_package == NULL) return OTA_ERR_ARGUMENT;
    status = read_package(io, request, package);
    return status == OTA_OK ? walk_image(io, request, package, 0) : status;
}

ota_status_t ota_package_validate(const ota_io_t *io, const ota_request_t *request, ota_image_t *out)
{
    package_t package = {0};
    ota_status_t status;
    if (out == NULL) return OTA_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    status = validate(io, request, &package);
    if (status == OTA_OK) *out = package.image;
    return status;
}

ota_status_t ota_package_validate_prefix(const ota_io_t *io, const ota_request_t *request)
{
    package_t package = {0};
    if (io == NULL || request == NULL || io->read_package == NULL) return OTA_ERR_ARGUMENT;
    return read_package(io, request, &package);
}

ota_status_t ota_install(const ota_io_t *io, const ota_request_t *request, ota_image_t *out)
{
    package_t package = {0};
    ota_status_t status;
    if (out == NULL) return OTA_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (io == NULL || io->mark_installing == NULL || io->erase_sector == NULL ||
        io->program_word == NULL || io->read_internal == NULL) return OTA_ERR_ARGUMENT;
    status = validate(io, request, &package);
    if (status != OTA_OK) return status;
    if (io->mark_installing(io->context, &package.image) != 0) return OTA_ERR_STATE;
    /* 一律擦目标槽共同容量的7个扇区；A2额外128KiB不触碰，B及当前槽不触碰。 */
    for (uint32_t offset = 0U; offset < OTA_IMAGE_LIMIT; offset += OTA_SECTOR_BYTES) {
        if (io->erase_sector(io->context, package.image.slot_base + offset) != 0) return OTA_ERR_ERASE;
    }
    status = walk_image(io, request, &package, 1);
    if (status != OTA_OK) return status;
    status = ota_installed_validate(io, &package.image);
    if (status != OTA_OK) return status;
    *out = package.image;
    return OTA_OK;
}

ota_status_t ota_installed_validate(const ota_io_t *io, const ota_image_t *image)
{
    uint8_t data[OTA_CHUNK_BYTES];
    uint32_t crc = 0U;
    if (io == NULL || io->read_internal == NULL || image == NULL ||
        (image->slot_base != OTA_A1_BASE && image->slot_base != OTA_A2_BASE) ||
        image->image_bytes < 8U || image->image_bytes > OTA_IMAGE_LIMIT) return OTA_ERR_ARGUMENT;
    for (uint32_t offset = 0U; offset < image->image_bytes; offset += sizeof(data)) {
        uint32_t count = image->image_bytes - offset;
        if (count > sizeof(data)) count = sizeof(data);
        if (io->read_internal(io->context, image->slot_base + offset, data, count) != 0) {
            return OTA_ERR_READ;
        }
        if (offset == 0U && !valid_vectors(data, image->slot_base, image->image_bytes)) {
            return OTA_ERR_VERIFY;
        }
        crc = ota_crc32(crc, data, count);
    }
    if (crc != image->image_crc32) return OTA_ERR_VERIFY;
    return OTA_OK;
}
