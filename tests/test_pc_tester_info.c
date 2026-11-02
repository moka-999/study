/* QEMU执行实际FRU读取/迁移/CRC/双副本，仅将Flash读写换成内存。 */
#include "pc_tester_info.h"
#include "flash_bsp.h"
#include "boot_fru.h"
#include "boot_vectors.h"
#include "board_storage_config.h"
#include <string.h>

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
#define CHECK(c) do { if (!(c)) fail("FAIL " STRINGIFY(__LINE__) ": " #c "\n"); } while (0)
static uint8_t storage[0x4000], original_storage[0x4000];
static unsigned writes, locked, lock_failure, read_failure, fail_read_after_write;
static int write_cut = -1;
static unsigned cut_at_write = 1U;

static void message(const char *text)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = text;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}
static _Noreturn void finish(uint32_t code)
{
    const uint32_t block[] = {0x20026U, code};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register const uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) { }
}
static _Noreturn void fail(const char *text) { message(text); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }

static int read_flash(flash_if_t *flash, uint32_t address, uint8_t *data, uint32_t length)
{
    (void)flash;
    CHECK(address <= sizeof(storage) && length <= sizeof(storage) - address);
    if (read_failure) return IF_ERR;
    memcpy(data, storage + address, length);
    return IF_OK;
}
static int write_flash(flash_if_t *flash, uint32_t address, const uint8_t *data, uint32_t length)
{
    (void)flash;
    CHECK(address == FLASH_LAYOUT_FRU_SLOT_A || address == FLASH_LAYOUT_FRU_SLOT_B ||
          address == FLASH_LAYOUT_VERSION_SLOT_A || address == FLASH_LAYOUT_VERSION_SLOT_B);
    CHECK(length <= FLASH_LAYOUT_SECTOR_SIZE);
    ++writes;
    memset(storage + address, 0xFF, FLASH_LAYOUT_SECTOR_SIZE);
    if (write_cut >= 0 && writes == cut_at_write) {
        CHECK((unsigned)write_cut <= length);
        memcpy(storage + address, data, (unsigned)write_cut);
        return IF_ERR;
    }
    memcpy(storage + address, data, length);
    if (fail_read_after_write) read_failure = 1U;
    return IF_OK;
}
flash_if_t flash_if = {.read = read_flash, .write = write_flash};
int flash_transaction_begin(uint32_t timeout_ms)
{
    CHECK(timeout_ms == 1000U && locked == 0U);
    if (lock_failure) return -1;
    locked = 1U;
    return 0;
}
void flash_transaction_end(void) { CHECK(locked == 1U); locked = 0U; }
const uint8_t *board_mac_address(void)
{
    static const uint8_t mac[6] = {2, 1, 2, 3, 4, 5};
    return mac;
}

/* 启动阶段负责迁移；测试以回读值验证，不把RAM临时默认值当作落盘结果。 */
static int prepare_and_load(fru_info_t *fru)
{
    if (boot_fru_prepare(&flash_if, board_mac_address()) != 0) return -1;
    return fru_info_load(&flash_if, fru, NULL) == FLASH_STORE_OK ? 0 : -1;
}

static fru_info_t seed(const char *name)
{
    fru_info_t value;
    fru_info_init(&value);
    strcpy(value.product_name, "LPDDR4/X_200B");
    strcpy(value.tester_name, name);
    strcpy(value.hardware_revision, "D00");
    strcpy(value.test_board, "RK3588_test");
    strcpy(value.serial_number, "KEEP-SERIAL-1234");
    strcpy(value.manufacturing_date, "2026-10-09");
    strcpy(value.site_label, "7");
    memcpy(value.mac_address, board_mac_address(), 6);
    value.network_mode = FRU_NETWORK_MODE_STATIC;
    memcpy(value.configured_ipv4, (uint8_t[]){192, 168, 1, 201}, 4);
    memcpy(value.configured_netmask, (uint8_t[]){255, 255, 255, 0}, 4);
    memcpy(value.configured_gateway, (uint8_t[]){192, 168, 1, 1}, 4);
    value.service_port = 8888;
    memset(value.reserved, 0xA6, sizeof(value.reserved));
    memset(storage, 0xFF, sizeof(storage));
    CHECK(fru_info_save(&flash_if, &value, NULL) == FLASH_STORE_OK);
    CHECK(fru_info_save(&flash_if, &value, NULL) == FLASH_STORE_OK);
    writes = 0U;
    return value;
}

/* 构造介质上的真实记录，可注入CRC正确但字段非法或版本未知的载荷。 */
static void put_record(uint32_t address, uint16_t type, const void *payload,
                       uint32_t length, uint32_t generation)
{
    flash_record_header_t header;
    CHECK(length <= FLASH_LAYOUT_SECTOR_SIZE - sizeof(header));
    memset(&header, 0xFF, sizeof(header));
    header.magic = FLASH_RECORD_MAGIC;
    header.format_version = FLASH_RECORD_FORMAT_VERSION;
    header.record_type = type;
    header.generation = generation;
    header.payload_length = length;
    header.payload_crc32 = flash_crc32(payload, length);
    header.header_crc32 = flash_crc32((const uint8_t *)&header,
                                     offsetof(flash_record_header_t, header_crc32));
    memset(storage + address, 0xFF, FLASH_LAYOUT_SECTOR_SIZE);
    memcpy(storage + address, &header, sizeof(header));
    memcpy(storage + address + sizeof(header), payload, length);
}

static void test_slot_validation(void)
{
    fru_info_t expected, actual, bad;
    uint32_t generation;
    for (unsigned fault = 0U; fault < 3U; ++fault) {
        expected = seed("CUSTOM-TESTER");
        bad = expected;
        if (fault == 0U) memset(bad.tester_name, 'X', sizeof(bad.tester_name));
        if (fault == 1U) bad.network_mode = 3U;
        put_record(FLASH_LAYOUT_FRU_SLOT_B, FLASH_RECORD_TYPE_FRU, &bad, sizeof(bad), 2U);
        if (fault == 2U) storage[FLASH_LAYOUT_FRU_SLOT_B + sizeof(flash_record_header_t) + 4U] ^= 1U;
        CHECK(fru_info_load(&flash_if, &actual, &generation) == FLASH_STORE_OK);
        CHECK(generation == 1U && memcmp(&actual, &expected, sizeof(actual)) == 0);
        CHECK(prepare_and_load(&actual) == 0 && writes == 0U);
    }
    /* 有效未知格式即使与可读副本并存，也不得默认重建或覆盖。 */
    for (unsigned fault = 0U; fault < 4U; ++fault) {
        expected = seed("CUSTOM-TESTER");
        bad = expected;
        if (fault == 0U) bad.schema_version = 3U;
        put_record(FLASH_LAYOUT_FRU_SLOT_B, FLASH_RECORD_TYPE_FRU, &bad, sizeof(bad), 2U);
        flash_record_header_t header;
        memcpy(&header, storage + FLASH_LAYOUT_FRU_SLOT_B, sizeof(header));
        if (fault == 1U) header.format_version = 2U;
        if (fault == 2U) header.record_type = 99U;
        if (fault == 3U) header.payload_length = FLASH_LAYOUT_SECTOR_SIZE;
        header.header_crc32 = flash_crc32((const uint8_t *)&header,
                                         offsetof(flash_record_header_t, header_crc32));
        memcpy(storage + FLASH_LAYOUT_FRU_SLOT_B, &header, sizeof(header));
        memcpy(original_storage, storage, sizeof(storage));
        CHECK(fru_info_load(&flash_if, &actual, NULL) == FLASH_STORE_ERR_UNSUPPORTED);
        CHECK(fru_info_save(&flash_if, &expected, NULL) == FLASH_STORE_ERR_UNSUPPORTED);
        CHECK(prepare_and_load(&actual) == -1 && writes == 0U && locked == 0U);
        CHECK(memcmp(storage, original_storage, sizeof(storage)) == 0);
    }
}

static void test_legacy_fru(void)
{
    for (unsigned layout = 0U; layout < 2U; ++layout) {
        fru_info_t expected = seed("CUSTOM-TESTER"), actual;
        uint8_t legacy[192] = {0};
        const unsigned length = layout == 0U ? 160U : 192U;
        uint32_t generation;
        strcpy(expected.product_name, "KEEP-PRODUCT");
        strcpy(expected.hardware_revision, "KEEP-HW");
        if (layout == 0U) {
            /* 160字节v1与v2共有前138字节，旧reserved紧跟site_label。 */
            memcpy(legacy, &expected, offsetof(fru_info_t, tester_name));
            memcpy(legacy + 138U, expected.reserved, sizeof(expected.reserved));
        } else {
            /* 192字节旧B1记录包含料号、MAC有效位，后续字段偏移不同。 */
            memcpy(legacy + 4U, expected.product_name, 32U);
            memcpy(legacy + 36U, "KEEP-PART", sizeof("KEEP-PART"));
            memcpy(legacy + 68U, expected.serial_number, 32U);
            memcpy(legacy + 100U, expected.hardware_revision, 16U);
            memcpy(legacy + 116U, expected.manufacturing_date, 16U);
            memcpy(legacy + 132U, expected.mac_address, 6U);
            legacy[138] = 1U;
            legacy[139] = expected.network_mode;
            memcpy(legacy + 140U, expected.configured_ipv4, 4U);
            memcpy(legacy + 144U, expected.configured_netmask, 4U);
            memcpy(legacy + 148U, expected.configured_gateway, 4U);
            memcpy(legacy + 152U, &expected.service_port, 2U);
            memcpy(legacy + 154U, expected.site_label, 16U);
            memcpy(legacy + 170U, expected.reserved, 22U);
        }
        legacy[0] = 1U; legacy[1] = 0U;
        legacy[2] = (uint8_t)length; legacy[3] = 0U;
        put_record(FLASH_LAYOUT_FRU_SLOT_B, FLASH_RECORD_TYPE_FRU, legacy, length, 20U);
        CHECK(fru_info_load(&flash_if, &actual, &generation) == FLASH_STORE_ERR_LEGACY);
        CHECK(generation == 20U && writes == 0U);
        memset(expected.tester_name, 0, sizeof(expected.tester_name));
        strcpy(expected.tester_name, "JMCT-4000");
        memcpy(original_storage, storage, sizeof(storage));
        /* 写入任意字节处中断，仍可读到原v1；重试保留身份并接续代数。 */
        for (unsigned cut = 0U; cut < sizeof(flash_record_header_t) + sizeof(fru_info_t); ++cut) {
            memcpy(storage, original_storage, sizeof(storage));
            writes = 0U;
            write_cut = (int)cut;
            CHECK(prepare_and_load(&actual) == -1 && locked == 0U);
            write_cut = -1;
            CHECK(fru_info_load(&flash_if, &actual, NULL) == FLASH_STORE_ERR_LEGACY);
            CHECK(prepare_and_load(&actual) == 0 && writes == 3U);
            CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0);
            CHECK(fru_info_load(&flash_if, &actual, &generation) == FLASH_STORE_OK);
            CHECK(generation == 22U);
        }
    }
}

static void test_version_compatibility(void)
{
    firmware_version_info_t expected = *firmware_version_current(), bad, actual;
    uint32_t generation;
    uint8_t legacy[96] = {1U, 0U, 96U, 0U};
    memset(storage, 0xFF, sizeof(storage));
    expected.version_patch = 42U;
    expected.image_length = 123456U;
    expected.image_crc32 = 0x12345678U;
    CHECK(firmware_version_save(&flash_if, &expected, NULL) == FLASH_STORE_OK);
    bad = expected;
    memset(bad.target, 'X', sizeof(bad.target));
    put_record(FLASH_LAYOUT_VERSION_SLOT_B, FLASH_RECORD_TYPE_FIRMWARE_VERSION,
               &bad, sizeof(bad), 2U);
    writes = 0U;
    CHECK(firmware_version_load(&flash_if, &actual, &generation) == FLASH_STORE_OK);
    CHECK(generation == 1U && memcmp(&actual, &expected, sizeof(actual)) == 0);
    /* 96字节旧记录只读转换，不覆盖W25，也不改用编译期版本身份。 */
    memcpy(legacy + 4U, &expected.version_major, 6U);
    memcpy(legacy + 16U, &expected.image_length, 4U);
    memcpy(legacy + 20U, &expected.image_crc32, 4U);
    memcpy(legacy + 24U, expected.build_time_utc, 24U);
    memcpy(legacy + 64U, expected.build_id, 16U);
    memcpy(legacy + 80U, expected.target, 16U);
    put_record(FLASH_LAYOUT_VERSION_SLOT_B, FLASH_RECORD_TYPE_FIRMWARE_VERSION,
               legacy, sizeof(legacy), 3U);
    memcpy(original_storage, storage, sizeof(storage));
    CHECK(firmware_version_load(&flash_if, &actual, &generation) == FLASH_STORE_OK);
    CHECK(generation == 3U && memcmp(&actual, &expected, sizeof(actual)) == 0);
    CHECK(writes == 0U && memcmp(storage, original_storage, sizeof(storage)) == 0);
    legacy[0] = 99U;
    put_record(FLASH_LAYOUT_VERSION_SLOT_B, FLASH_RECORD_TYPE_FIRMWARE_VERSION,
               legacy, sizeof(legacy), 4U);
    memcpy(original_storage, storage, sizeof(storage));
    CHECK(firmware_version_load(&flash_if, &actual, NULL) == FLASH_STORE_ERR_UNSUPPORTED);
    CHECK(firmware_version_save(&flash_if, &expected, NULL) == FLASH_STORE_ERR_UNSUPPORTED);
    CHECK(writes == 0U && memcmp(storage, original_storage, sizeof(storage)) == 0);
}

static void test_read_only_query(void)
{
    fru_info_t actual, expected = seed("LPDDR4/X_200B");
    /* 查询不迁移、不改名，也不要求接口提供写函数。 */
    flash_if.write = NULL;
    CHECK(pc_tester_info_load(&actual) == 0 && writes == 0U);
    CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0);
    CHECK(fru_info_save(&flash_if, &expected, NULL) == FLASH_STORE_ERR_PARAM);
    memset(storage, 0xFF, sizeof(storage));
    CHECK(pc_tester_info_load(&actual) == -1 && writes == 0U);
    CHECK(boot_fru_prepare(NULL, NULL) == -1);
    flash_if.write = write_flash;
}

static void test_first_boot_power_cuts(void)
{
    fru_info_t actual;
    for (cut_at_write = 1U; cut_at_write <= 2U; ++cut_at_write) {
        for (unsigned cut = 0U; cut < 256U; ++cut) {
            memset(storage, 0xFF, sizeof(storage));
            memset(storage, 0xA6, FLASH_LAYOUT_FRU_SLOT_A);
            writes = 0U; write_cut = (int)cut;
            CHECK(boot_fru_prepare(&flash_if, board_mac_address()) == -1);
            write_cut = -1;
            CHECK(prepare_and_load(&actual) == 0);
            CHECK(strcmp(actual.tester_name, TESTER_NAME) == 0);
            CHECK(memcmp(actual.mac_address, board_mac_address(), 6U) == 0);
            CHECK(actual.serial_number[0] == 0 && actual.site_label[0] == 0);
            for (unsigned i = 0U; i < FLASH_LAYOUT_FRU_SLOT_A; ++i) CHECK(storage[i] == 0xA6U);
            unsigned before = writes;
            CHECK(prepare_and_load(&actual) == 0 && writes == before);
        }
    }
    cut_at_write = 1U;
}

static void test_vectors(void)
{
    CHECK(boot_app_vectors_valid(0x20020000U, 0x08020299U));
    CHECK(!boot_app_vectors_valid(0x20020001U, 0x08020299U));
    CHECK(!boot_app_vectors_valid(0x20000000U, 0x08020299U));
    CHECK(!boot_app_vectors_valid(0x24020000U, 0x08020299U));
    CHECK(!boot_app_vectors_valid(0x20020000U, 0x08000001U));
    CHECK(!boot_app_vectors_valid(0x20020000U, 0x08020298U));
    CHECK(!boot_app_vectors_valid(0x20020000U, 0x08200001U));
}

void test_main(void)
{
    test_vectors();
#if !BOARD_W25_PRESENT
    /* 模拟响应不能获取Flash锁或发起Flash读写；所有后台入口故意失败。 */
    fru_info_t mock;
    lock_failure = read_failure = 1U;
    flash_if.read = NULL; flash_if.write = NULL;
    CHECK(pc_tester_info_load(&mock) == 0);
    CHECK(strcmp(mock.tester_name, TESTER_NAME) == 0);
    CHECK(strcmp(mock.hardware_revision, TESTER_HARDWARE_REVISION) == 0);
    CHECK(strcmp(mock.test_board, TESTER_BOARD_NAME) == 0);
    CHECK(mock.serial_number[0] == 0 && mock.site_label[0] == 0);
    CHECK(writes == 0U && locked == 0U && pc_tester_info_load(NULL) == -1);
    message("PASS: absent W25 mock info, no Flash access, vector boundaries\n");
    finish(0U);
#endif
    fru_info_t actual, expected = seed("LPDDR4/X_200B");
    uint32_t generation = 0U;
    memcpy(original_storage, storage, sizeof(storage));
    memset(expected.tester_name, 0, sizeof(expected.tester_name));
    strcpy(expected.tester_name, "JMCT-4000");
    CHECK(prepare_and_load(&actual) == 0);
    CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0);
    CHECK(writes == 1U && locked == 0U);
    CHECK(fru_info_load(&flash_if, &actual, &generation) == FLASH_STORE_OK);
    CHECK(generation == 3U && memcmp(&actual, &expected, sizeof(actual)) == 0);
    CHECK(memcmp(storage, original_storage, FLASH_LAYOUT_FRU_SLOT_A) == 0);
    CHECK(memcmp(storage + FLASH_LAYOUT_FRU_SLOT_B,
                 original_storage + FLASH_LAYOUT_FRU_SLOT_B, FLASH_LAYOUT_SECTOR_SIZE) == 0);
    CHECK(prepare_and_load(&actual) == 0 && writes == 1U);

    /* 逐字节模拟迁移写入中断：旧副本不丢，恢复后可再次迁移。 */
    for (unsigned cut = 0U; cut < sizeof(flash_record_header_t) + sizeof(fru_info_t); ++cut) {
        memcpy(storage, original_storage, sizeof(storage));
        writes = 0U;
        write_cut = (int)cut;
        CHECK(prepare_and_load(&actual) == -1 && locked == 0U);
        write_cut = -1;
        CHECK(fru_info_load(&flash_if, &actual, NULL) == FLASH_STORE_OK);
        CHECK(strcmp(actual.tester_name, "LPDDR4/X_200B") == 0);
        CHECK(prepare_and_load(&actual) == 0 && writes == 2U);
        CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0);
    }

    /* 任意非旧默认名称保持原值；不把合法定制记录强制改成编译期常量。 */
    expected = seed("CUSTOM-TESTER");
    CHECK(prepare_and_load(&actual) == 0 && writes == 0U);
    CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0);
    expected = seed("JMCT-4000");
    CHECK(prepare_and_load(&actual) == 0 && writes == 0U);
    CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0);

    (void)seed("LPDDR4/X_200B");
    lock_failure = 1U;
    CHECK(pc_tester_info_load(&actual) == -1 && writes == 0U && locked == 0U);
    lock_failure = 0U;
    read_failure = 1U;
    CHECK(prepare_and_load(&actual) == -1 && writes == 0U && locked == 0U);
    read_failure = 0U;
    fail_read_after_write = 1U;
    CHECK(prepare_and_load(&actual) == -1 && writes == 1U && locked == 0U);
    fail_read_after_write = read_failure = 0U;
    CHECK(prepare_and_load(&actual) == 0 && writes == 1U);
    CHECK(strcmp(actual.tester_name, "JMCT-4000") == 0);
    CHECK(pc_tester_info_load(NULL) == -1 && locked == 0U);

    memset(storage, 0xFF, sizeof(storage));
    writes = 0U;
    CHECK(prepare_and_load(&actual) == 0 && writes == 2U);
    CHECK(strcmp(actual.tester_name, "JMCT-4000") == 0);
    CHECK(strcmp(actual.product_name, "LPDDR4/X_200B") == 0);
    CHECK(strcmp(actual.hardware_revision, "D00") == 0);
    CHECK(strcmp(actual.test_board, "RK3588_test") == 0);
    CHECK(prepare_and_load(&actual) == 0 && writes == 2U);
    test_read_only_query();
    test_first_boot_power_cuts();
    test_slot_validation();
    test_legacy_fru();
    test_version_compatibility();
    message("PASS: actual tester FRU migration, 1280 power cuts, unchanged-write skip, unknown format protection, legacy FRU/version\n");
    finish(0U);
}
