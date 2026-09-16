/* 直接执行boot/ota_install.c；Flash只用RAM模拟，不模拟QSPI/电压/ECC物理行为。 */
#include "ota_install.h"

#include <stddef.h>
#include <string.h>

#define FLASH_BASE UINT32_C(0x08000000)
#define FLASH_BYTES UINT32_C(0x00200000)
#define PACKAGE ((uint8_t *)UINT32_C(0x60000000))
#define EXPECTED ((const uint8_t *)UINT32_C(0x60200000))
#define CONFIG ((const uint32_t *)UINT32_C(0x60400000))
#define TEXT_INNER(x) #x
#define TEXT(x) TEXT_INNER(x)
#define CHECK(c) do { if (!(c)) { fail("FAIL line " TEXT(__LINE__) ": " #c "\n"); } } while (0)

/* 模拟整颗内部Flash，所有非目标地址填保护哨兵，用于发现越界擦写。 */
static uint8_t internal_flash[FLASH_BYTES];
static uint32_t read_calls, erase_calls, program_calls, internal_calls, marked;
static uint32_t target_base, fault_mode, fault_index, package_bytes, source_base;

/** 半主机仅打印并退出测试进程；不让待安装的App获得执行权。 */
static void message(const char *text)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = text;
    __asm__ volatile ("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}

static _Noreturn void finish(uint32_t code)
{
    const uint32_t block[2] = {0x20026U, code};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register const uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile ("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) { }
}

static _Noreturn void fail(const char *text)
{
    message(text);
    finish(1U);
}

void test_abort(void)
{
    fail("FAIL: emulated CPU exception\n");
}

/** 只在已提交INSTALLING之后，才准访问目标槽的擦写/验证接口。 */
static uint8_t *target_address(uint32_t address, uint32_t bytes)
{
    CHECK(marked != 0U);
    CHECK(address >= target_base && address < target_base + OTA_IMAGE_LIMIT);
    CHECK(bytes <= target_base + OTA_IMAGE_LIMIT - address);
    return internal_flash + address - FLASH_BASE;
}

static int read_package(void *context, uint32_t address, uint8_t *data, uint32_t bytes)
{
    (void)context;
    ++read_calls;
    CHECK(address >= source_base && address <= source_base + package_bytes);
    CHECK(bytes <= source_base + package_bytes - address);
    if (fault_mode == 4U && read_calls == fault_index) return -1;
    memcpy(data, PACKAGE + address - source_base, bytes);
    return 0;
}

static int mark_installing(void *context, const ota_image_t *image)
{
    (void)context;
    CHECK(marked == 0U && erase_calls == 0U && program_calls == 0U);
    CHECK(image->slot_base == target_base && image->image_bytes <= OTA_IMAGE_LIMIT);
    if (fault_mode == 5U) return -1;
    marked = 1U;
    /* 模拟安装前校验通过后源文件变化；必须最终拒绝，不准报告目标有效。 */
    if (fault_mode == 7U) PACKAGE[package_bytes - 5U] ^= 0x40U;
    return 0;
}

static int erase_sector(void *context, uint32_t address)
{
    (void)context;
    uint8_t *data = target_address(address, OTA_SECTOR_BYTES);
    CHECK(address % OTA_SECTOR_BYTES == 0U);
    ++erase_calls;
    /* 故意先擦半扇区再报错，不能仅测试完全没改数据的理想失败。 */
    if (fault_mode == 2U && erase_calls == fault_index) {
        memset(data, 0xFF, OTA_SECTOR_BYTES / 2U);
        return -1;
    }
    memset(data, 0xFF, OTA_SECTOR_BYTES);
    return 0;
}

static int program_word(void *context, uint32_t address, const uint8_t *source)
{
    (void)context;
    uint8_t *data = target_address(address, OTA_FLASH_WORD_BYTES);
    CHECK(address % OTA_FLASH_WORD_BYTES == 0U);
    CHECK((uintptr_t)source % OTA_FLASH_WORD_BYTES == 0U);
    ++program_calls;
    for (uint32_t i = 0U; i < OTA_FLASH_WORD_BYTES; ++i) {
        CHECK(data[i] == 0xFFU); /* 同一次擦除后，禁止重复写Flash-word。 */
        data[i] = source[i];
        if (fault_mode == 3U && program_calls == fault_index && i == 15U) return -1;
    }
    return 0;
}

static int read_internal(void *context, uint32_t address, uint8_t *data, uint32_t bytes)
{
    (void)context;
    ++internal_calls;
    if (fault_mode == 8U && internal_calls == fault_index) return -1;
    memcpy(data, target_address(address, bytes), bytes);
    if (fault_mode == 6U && internal_calls == 1U) data[0] ^= 0x08U;
    if (fault_mode == 9U && internal_calls == 1U) data[bytes - 1U] ^= 0x80U;
    return 0;
}

static void check_protected(void)
{
    for (uint32_t i = 0U; i < FLASH_BYTES; ++i) {
        uint32_t address = FLASH_BASE + i;
        if (address < target_base || address >= target_base + OTA_IMAGE_LIMIT || marked == 0U) {
            CHECK(internal_flash[i] == 0xA6U);
        }
    }
}

void test_main(void)
{
    ota_request_t request = {0};
    ota_image_t result;
    ota_io_t io = {NULL, read_package, mark_installing, erase_sector, program_word, read_internal};
    ota_status_t status;
    const ota_image_t empty = {0};
    package_bytes = CONFIG[0];
    request.package_bytes = package_bytes;
    request.package_header_crc32 = CONFIG[1];
    request.confirmed_base = CONFIG[2];
    request.current_version[0] = (uint16_t)CONFIG[6];
    request.current_version[1] = (uint16_t)CONFIG[7];
    request.current_version[2] = (uint16_t)CONFIG[8];
    request.recovery = CONFIG[10];
    memcpy(request.target, "apollo-v2-h743", sizeof("apollo-v2-h743"));
    source_base = request.recovery == 1U ? OTA_STABLE_BASE : OTA_DOWNLOAD_BASE;
    target_base = request.recovery == 1U ? OTA_A1_BASE :
                  request.confirmed_base == OTA_A1_BASE ? OTA_A2_BASE : OTA_A1_BASE;
    fault_mode = CONFIG[4];
    fault_index = CONFIG[5];
    memset(internal_flash, 0xA6, sizeof(internal_flash));
    CHECK(ota_crc32(0U, (const uint8_t *)"123456789", 9U) == UINT32_C(0xCBF43926));
    CHECK(ota_crc32(ota_crc32(0U, (const uint8_t *)"1234", 4U),
                    (const uint8_t *)"56789", 5U) == UINT32_C(0xCBF43926));
    CHECK(ota_crc32(0x12345678U, NULL, 0U) == 0x12345678U);
    /* 先检查纯读取入口，不允许顺便提交状态或擦除。 */
    if (CONFIG[3] == OTA_OK) {
        CHECK(ota_package_validate(&io, &request, &result) == OTA_OK);
        CHECK(marked == 0U && erase_calls == 0U && program_calls == 0U);
        read_calls = 0U;
    }
    memset(&result, 0xDA, sizeof(result));
    status = ota_install(&io, &request, &result);
    CHECK(status == (ota_status_t)CONFIG[3]);
    check_protected();
    if (status == OTA_OK) {
        uint32_t offset = target_base - FLASH_BASE;
        CHECK(result.slot_base == target_base && marked == 1U && erase_calls == 7U);
        CHECK(result.image_bytes == CONFIG[9]);
        CHECK(program_calls == (result.image_bytes + 31U) / 32U);
        CHECK(memcmp(internal_flash + offset, EXPECTED, result.image_bytes) == 0);
        CHECK(ota_crc32(0U, EXPECTED, result.image_bytes) == result.image_crc32);
        CHECK(memcmp(result.version_payload + 16U, &result.image_crc32, 4U) == 0);
        for (uint32_t i = result.image_bytes; i < OTA_IMAGE_LIMIT; ++i) {
            CHECK(internal_flash[offset + i] == 0xFFU);
        }
    } else {
        CHECK(memcmp(&result, &empty, sizeof(result)) == 0);
        if (fault_mode == 0U || fault_mode == 5U) {
            CHECK(marked == 0U && erase_calls == 0U && program_calls == 0U);
        }
        if (fault_mode == 2U) CHECK(erase_calls == fault_index && program_calls == 0U);
        if (fault_mode == 3U) CHECK(program_calls == fault_index);
        /* 模拟重新上电后仍由日志给出相同来源/目标；必须重擦原目标并成功恢复。 */
        if (fault_mode == 2U || fault_mode == 3U) {
            fault_mode = 0U;
            marked = read_calls = erase_calls = program_calls = internal_calls = 0U;
            CHECK(ota_install(&io, &request, &result) == OTA_OK);
            check_protected();
            CHECK(memcmp(internal_flash + target_base - FLASH_BASE, EXPECTED, result.image_bytes) == 0);
        }
    }
    /* 空指针/缺少持久化操作都须先拒绝，不能降级为“没有日志也继续刷”。 */
    CHECK(ota_install(NULL, &request, &result) == OTA_ERR_ARGUMENT);
    CHECK(ota_install(&io, NULL, &result) == OTA_ERR_ARGUMENT);
    CHECK(ota_install(&io, &request, NULL) == OTA_ERR_ARGUMENT);
    io.mark_installing = NULL;
    CHECK(ota_install(&io, &request, &result) == OTA_ERR_ARGUMENT);
    message("PASS: C installer and protected ranges\n");
    finish(0U);
}
