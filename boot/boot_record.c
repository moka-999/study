#include "boot_record.h"

#include <string.h>

#define RECORD_MAGIC UINT32_C(0x31545342) /* 字节BST1。 */
#define RECORD_FORMAT UINT32_C(0x01000001) /* 高16位记录长度256，低16位格式1。 */
#define RECORD_COMMIT UINT32_C(0xC001CAFE)
#define RECORD_COUNT (2U * BOOT_JOURNAL_SECTOR / BOOT_RECORD_BYTES)

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static void put32(uint8_t *p, uint32_t value)
{
    for (uint32_t i = 0; i < 4U; ++i) p[i] = (uint8_t)(value >> (8U * i));
}

uint32_t boot_slot_base(uint32_t slot)
{
    return slot == 0U ? OTA_A1_BASE : slot == 1U ? OTA_A2_BASE : 0U;
}

/** 除CRC外再核对状态语义；损坏记录不能借合法CRC授权擦写地址。 */
static int valid(const boot_record_t *r)
{
    if (r->phase > BOOT_MAINTENANCE || r->active > 1U ||
        (r->previous > 1U && r->previous != BOOT_SLOT_NONE) ||
        r->previous == r->active || r->valid_mask > 3U || r->pending > 1U ||
        r->stage > 3U || r->failures > 3U || r->install_tries > 3U ||
        r->package_bytes > OTA_PACKAGE_LIMIT) return 0;
    if ((r->phase == BOOT_NORMAL || r->phase == BOOT_TRIAL || r->phase == BOOT_READY) &&
        r->stage < 2U && (r->valid_mask & (1U << r->active)) == 0U) return 0;
    if (r->pending != 0U && r->phase != BOOT_NORMAL && r->phase != BOOT_TRIAL) return 0;
    if ((r->phase == BOOT_READY || (r->phase == BOOT_INSTALLING && r->stage < 2U)) &&
        r->package_bytes < OTA_PREFIX_BYTES) return 0;
    for (uint32_t i = 0; i < 2U; ++i) {
        if ((r->valid_mask & (1U << i)) == 0U) continue;
        const ota_image_t *im = &r->images[i];
        if (im->slot_base != boot_slot_base(i) || im->image_bytes < 8U ||
            im->image_bytes > OTA_IMAGE_LIMIT ||
            get32(im->version_payload) != UINT32_C(0x004C0001) ||
            get32(im->version_payload + 12) != im->image_bytes ||
            get32(im->version_payload + 16) != im->image_crc32) return 0;
    }
    return 1;
}

/** 明确逐字段编码，避免主机/编译器结构体填充变化破坏持久化ABI。 */
static void encode(const boot_record_t *r, uint8_t *p)
{
    const uint32_t fields[] = { RECORD_MAGIC, RECORD_FORMAT, r->generation, r->phase,
        r->active, r->previous, r->valid_mask, r->pending, r->stage, r->failures,
        r->install_tries, r->package_bytes, r->package_crc, r->epoch, r->last_error, 0U };
    for (uint32_t i = 0; i < 16U; ++i) put32(p + 4U * i, fields[i]);
    for (uint32_t i = 0; i < 2U; ++i) {
        const ota_image_t *im = &r->images[i];
        uint8_t *q = p + 64U + i * 92U;
        put32(q, im->slot_base);
        put32(q + 4, im->image_bytes);
        put32(q + 8, im->image_crc32);
        put32(q + 12, im->package_header_crc32);
        memcpy(q + 16, im->version_payload, OTA_VERSION_BYTES);
    }
    put32(p + 248, ota_crc32(0U, p, 248U));
    put32(p + 252, RECORD_COMMIT);
}

static int decode(const uint8_t *p, boot_record_t *r)
{
    if (get32(p) != RECORD_MAGIC || get32(p + 4) != RECORD_FORMAT || get32(p + 60) != 0U ||
        get32(p + 252) != RECORD_COMMIT || get32(p + 248) != ota_crc32(0U, p, 248U)) return 0;
    r->generation = get32(p + 8); r->phase = get32(p + 12);
    r->active = get32(p + 16); r->previous = get32(p + 20);
    r->valid_mask = get32(p + 24); r->pending = get32(p + 28);
    r->stage = get32(p + 32); r->failures = get32(p + 36);
    r->install_tries = get32(p + 40); r->package_bytes = get32(p + 44);
    r->package_crc = get32(p + 48); r->epoch = get32(p + 52); r->last_error = get32(p + 56);
    for (uint32_t i = 0; i < 2U; ++i) {
        ota_image_t *im = &r->images[i];
        const uint8_t *q = p + 64U + i * 92U;
        im->slot_base = get32(q); im->image_bytes = get32(q + 4);
        im->image_crc32 = get32(q + 8); im->package_header_crc32 = get32(q + 12);
        memcpy(im->version_payload, q + 16, OTA_VERSION_BYTES);
    }
    return valid(r);
}

static int erased(const uint8_t *data, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) if (data[i] != 0xFFU) return 0;
    return 1;
}

/** 代数回绕采用半空间比较；重复代数或半空间歧义视为异常，不能任意选一份。 */
static int scan(const boot_store_t *s, boot_record_t *out, uint32_t *latest)
{
    uint8_t data[BOOT_RECORD_BYTES];
    boot_record_t candidate;
    int found = 0, blank = 1;
    memset(out, 0, sizeof(*out));
    if (s == NULL || s->read == NULL) return BOOT_STORE_IO;
    for (uint32_t i = 0; i < RECORD_COUNT; ++i) {
        if (s->read(s->context, BOOT_JOURNAL_BASE + i * BOOT_RECORD_BYTES,
                    data, sizeof(data)) != 0) return BOOT_STORE_IO;
        if (!erased(data, sizeof(data))) blank = 0;
        if (!decode(data, &candidate)) continue;
        if (found) {
            uint32_t delta = candidate.generation - out->generation;
            if (delta == 0U || delta == UINT32_C(0x80000000)) return BOOT_STORE_INVALID;
            if (delta > UINT32_C(0x80000000)) continue;
        }
        *out = candidate; *latest = i; found = 1;
    }
    return found ? BOOT_STORE_OK : blank ? BOOT_STORE_EMPTY : BOOT_STORE_INVALID;
}

int boot_record_load(const boot_store_t *store, boot_record_t *record)
{
    uint32_t latest = 0U;
    if (record == NULL) return BOOT_STORE_INVALID;
    int result = scan(store, record, &latest);
    if (result != BOOT_STORE_OK) memset(record, 0, sizeof(*record));
    return result;
}

int boot_record_save(const boot_store_t *s, boot_record_t *record)
{
    boot_record_t old, next;
    uint8_t data[BOOT_RECORD_BYTES], verify[BOOT_RECORD_BYTES];
    uint32_t latest = 0U, index = 0U;
    if (s == NULL || s->program == NULL || s->erase == NULL || record == NULL || !valid(record)) {
        return BOOT_STORE_INVALID;
    }
    int result = scan(s, &old, &latest);
    if (result != BOOT_STORE_OK && result != BOOT_STORE_EMPTY) return result;
    if ((result == BOOT_STORE_OK && old.generation != record->generation) ||
        (result == BOOT_STORE_EMPTY && record->generation != 0U)) return BOOT_STORE_STALE;
    if (result == BOOT_STORE_OK) {
        uint32_t end = (latest / 16U + 1U) * 16U;
        for (index = latest + 1U; index < end; ++index) {
            if (s->read(s->context, BOOT_JOURNAL_BASE + index * BOOT_RECORD_BYTES,
                        verify, sizeof(verify)) != 0) return BOOT_STORE_IO;
            if (erased(verify, sizeof(verify))) break;
        }
        if (index == end) {
            index = latest < 16U ? 16U : 0U;
            if (s->erase(s->context, BOOT_JOURNAL_BASE + index * BOOT_RECORD_BYTES) != 0) {
                return BOOT_STORE_IO;
            }
            /* 核对整扇区，不能在部分擦除态上开始一轮新日志。 */
            for (uint32_t i = 0; i < 16U; ++i) {
                if (s->read(s->context, BOOT_JOURNAL_BASE + (index + i) * BOOT_RECORD_BYTES,
                            verify, sizeof(verify)) != 0 || !erased(verify, sizeof(verify))) {
                    return BOOT_STORE_IO;
                }
            }
        }
    }
    next = *record; next.generation++;
    encode(&next, data);
    uint32_t address = BOOT_JOURNAL_BASE + index * BOOT_RECORD_BYTES;
    if (s->program(s->context, address, data, 252U) != 0 ||
        s->read(s->context, address, verify, 252U) != 0 || memcmp(data, verify, 252U) != 0 ||
        s->program(s->context, address + 252U, data + 252U, 4U) != 0 ||
        s->read(s->context, address, verify, sizeof(verify)) != 0 ||
        memcmp(data, verify, sizeof(data)) != 0 || !decode(verify, &old)) return BOOT_STORE_IO;
    *record = next;
    return BOOT_STORE_OK;
}

void boot_record_fallback(boot_record_t *r)
{
    r->valid_mask &= ~(1U << r->active);
    r->pending = 0U; r->failures = 0U; r->install_tries = 0U;
    r->package_bytes = 0U; r->package_crc = 0U;
    if (r->stage == 0U && r->previous < 2U && (r->valid_mask & (1U << r->previous)) != 0U) {
        r->active = r->previous; r->previous = BOOT_SLOT_NONE;
        r->stage = 1U; r->phase = BOOT_TRIAL;
    } else if (r->stage < 2U) {
        r->previous = BOOT_SLOT_NONE; r->stage = 2U; r->phase = BOOT_INSTALLING;
    } else {
        r->stage = 3U; r->phase = BOOT_MAINTENANCE;
    }
}

void boot_record_failed_start(boot_record_t *r)
{
    if (r->pending == 0U) return;
    r->pending = 0U;
    if (++r->failures >= 3U) boot_record_fallback(r);
}

int boot_record_confirm(boot_record_t *r, uint32_t base, uint32_t epoch)
{
    if ((r->phase != BOOT_NORMAL && r->phase != BOOT_TRIAL) || r->pending != 1U ||
        r->epoch != epoch || boot_slot_base(r->active) != base ||
        (r->valid_mask & (1U << r->active)) == 0U) return BOOT_STORE_STALE;
    r->pending = 0U; r->phase = BOOT_NORMAL; r->stage = 0U;
    r->failures = 0U; r->install_tries = 0U;
    r->package_bytes = 0U; r->package_crc = 0U;
    return BOOT_STORE_OK;
}
