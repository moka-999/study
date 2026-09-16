/* 执行正式boot_main与安装/日志代码；只将板级IO替换为RAM，复位用setjmp边界模拟。 */
#include "boot_port.h"
#include <setjmp.h>
#include <string.h>

#define PACKAGE ((const uint8_t *)0x60000000U)
#define STABLE ((const uint8_t *)0x60600000U)
#define FLASH_BASE 0x08000000U
#define S1(x) #x
#define S(x) S1(x)
#define CHECK(c) do { if (!(c)) die("FAIL " S(__LINE__) ": " #c "\n"); } while (0)
static uint8_t nor[0x6000], flash[0x200000];
static uint32_t allowed, jumped, stopped, erased, programmed, info_writes, cut_program;
static jmp_buf reset_boundary;
volatile uint32_t boot_diag[8];
extern void boot_main(void);

static void message(const char *s)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = s;
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
static _Noreturn void die(const char *s) { message(s); finish(1U); }
void test_abort(void) { die("FAIL CPU exception\n"); }
static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int boot_port_init(void) { allowed = jumped = stopped = 0U; return 0; }
void boot_allow_target(uint32_t base) { allowed = base; }
_Noreturn void boot_maintenance(uint32_t error) { stopped = error; longjmp(reset_boundary, 2); }
_Noreturn void boot_jump(uint32_t base) { jumped = base; longjmp(reset_boundary, 1); }

int boot_nor_read(void *ctx, uint32_t address, uint8_t *data, uint32_t size)
{
    (void)ctx;
    const uint8_t *src;
    if (address < sizeof(nor)) {
        CHECK(size <= sizeof(nor) - address); src = nor + address;
    } else {
        uint32_t base = address >= OTA_STABLE_BASE ? OTA_STABLE_BASE : OTA_DOWNLOAD_BASE;
        const uint8_t *package = base == OTA_STABLE_BASE ? STABLE : PACKAGE;
        CHECK(address >= base && address - base <= u32(package + 12));
        CHECK(size <= u32(package + 12) - (address - base));
        src = package + address - base;
    }
    memcpy(data, src, size); return 0;
}
int boot_nor_program(void *ctx, uint32_t address, const uint8_t *data, uint32_t size)
{
    (void)ctx;
    CHECK(address < sizeof(nor) && size <= sizeof(nor) - address);
    CHECK(address + size <= 0x2000U || address >= BOOT_JOURNAL_BASE);
    if (address < 0x2000U) ++info_writes;
    for (uint32_t i = 0; i < size; ++i) {
        CHECK((nor[address + i] & data[i]) == data[i]); nor[address + i] &= data[i];
    }
    return 0;
}
int boot_nor_erase(void *ctx, uint32_t address)
{
    (void)ctx;
    CHECK(address == 0U || address == 0x1000U || address == 0x4000U || address == 0x5000U);
    memset(nor + address, 255, 4096); return 0;
}
static const boot_store_t store = {NULL, boot_nor_read, boot_nor_program, boot_nor_erase};
static boot_record_t loaded(void)
{
    boot_record_t r; CHECK(boot_record_load(&store, &r) == 0); return r;
}
int boot_flash_read(void *ctx, uint32_t address, uint8_t *data, uint32_t size)
{
    (void)ctx; CHECK(address >= OTA_A1_BASE && address + size <= 0x081E0000U);
    memcpy(data, flash + address - FLASH_BASE, size); return 0;
}
int boot_flash_erase(void *ctx, uint32_t address)
{
    (void)ctx;
    CHECK(allowed && address >= allowed && address + OTA_SECTOR_BYTES <= allowed + OTA_IMAGE_LIMIT);
    boot_record_t r = loaded();
    CHECK(r.phase == BOOT_INSTALLING && !(r.valid_mask & (1U << (allowed == OTA_A1_BASE ? 0 : 1))));
    memset(flash + address - FLASH_BASE, 255, OTA_SECTOR_BYTES); ++erased; return 0;
}
int boot_flash_program(void *ctx, uint32_t address, const uint8_t *data)
{
    (void)ctx;
    CHECK(allowed && address >= allowed && address + 32U <= allowed + OTA_IMAGE_LIMIT);
    uint8_t *p = flash + address - FLASH_BASE;
    for (uint32_t i = 0; i < 32U; ++i) CHECK(p[i] == 255U);
    memcpy(p, data, 32); ++programmed;
    if (cut_program) { cut_program = 0U; longjmp(reset_boundary, 3); }
    return 0;
}
static int restart(void)
{
    int event = setjmp(reset_boundary);
    if (!event) boot_main();
    /* Boot和所有恢复路径都不得写FRU；B与A2末尾128KiB不能被安装器覆盖。 */
    for (uint32_t i = 0x2000; i < 0x4000; ++i) CHECK(nor[i] == 0x3CU);
    for (uint32_t i = 0; i < 0x20000; ++i) CHECK(flash[i] == 0xA6U);
    for (uint32_t i = 0x1E0000; i < sizeof(flash); ++i) CHECK(flash[i] == 0xA6U);
    return event;
}
static void seed(uint32_t slot)
{
    memset(nor, 255, sizeof(nor)); memset(nor + 0x2000, 0x3C, 8192);
    memset(flash, 0xA6, sizeof(flash)); erased = programmed = info_writes = cut_program = 0U;
    boot_record_t r = {0}; r.active = slot; r.previous = BOOT_SLOT_NONE; r.valid_mask = 1U << slot;
    ota_image_t *im = &r.images[slot]; im->slot_base = boot_slot_base(slot); im->image_bytes = 128U;
    uint8_t *a = flash + im->slot_base - FLASH_BASE;
    uint32_t vectors[] = {0x20020000U, im->slot_base + 65U}; memcpy(a, vectors, 8);
    im->image_crc32 = ota_crc32(0, a, im->image_bytes);
    im->version_payload[0] = 1U; im->version_payload[2] = 76U; im->version_payload[6] = 1U;
    memcpy(im->version_payload + 12, &im->image_bytes, 4); memcpy(im->version_payload + 16, &im->image_crc32, 4);
#if defined(BOOT_BOARD_B1)
    memcpy(im->version_payload + 60, "b1-h743", 8);
#else
    memcpy(im->version_payload + 60, "apollo-v2-h743", 14);
#endif
    CHECK(boot_record_save(&store, &r) == 0);
}
static void confirm(void)
{
    boot_record_t r = loaded(); CHECK(r.pending == 1U);
    CHECK(boot_record_confirm(&r, jumped, r.epoch) == 0); CHECK(boot_record_save(&store, &r) == 0);
}
static void request_install(uint32_t crc)
{
    boot_record_t r = loaded(); r.phase = BOOT_READY;
    r.package_bytes = u32(PACKAGE + 12); r.package_crc = crc;
    CHECK(boot_record_save(&store, &r) == 0);
}
void test_main(void)
{
    seed(0); memset(nor + 0x4000, 255, 8192);
    CHECK(restart() == 2 && stopped == 0x1002U && erased == 0U);
    seed(0); CHECK(restart() == 1 && jumped == OTA_A1_BASE); confirm();
    uint32_t writes = info_writes;
    CHECK(restart() == 1 && info_writes == writes && erased == 0U); confirm();
    request_install(u32(PACKAGE + 36));
    CHECK(restart() == 1 && jumped == OTA_A2_BASE && erased == 7U);
    for (uint32_t i = 1; i <= 6; ++i) {
        CHECK(restart() == 1);
        boot_record_t r = loaded();
        CHECK(r.stage == (i < 3 ? 0U : i < 6 ? 1U : 2U));
        CHECK(jumped == (i < 3 ? OTA_A2_BASE : OTA_A1_BASE));
    }
    CHECK(erased == 14U); /* 第二次安装来自稳定源，固定A1并允许降级。 */
    CHECK(restart() == 1); CHECK(restart() == 1); CHECK(restart() == 2);
    CHECK(loaded().phase == BOOT_MAINTENANCE && loaded().stage == 3U);

    seed(1); CHECK(restart() == 1); confirm(); request_install(u32(PACKAGE + 36));
    cut_program = 1U; CHECK(restart() == 3);
    CHECK(loaded().phase == BOOT_INSTALLING && loaded().active == 1U && loaded().valid_mask == 2U);
    CHECK(restart() == 1 && jumped == OTA_A1_BASE && erased == 14U); confirm();
    CHECK(loaded().phase == BOOT_NORMAL && loaded().pending == 0U);

    seed(0); CHECK(restart() == 1); confirm(); request_install(0U);
    CHECK(restart() == 1 && jumped == OTA_A1_BASE && erased == 0U);
    CHECK(loaded().last_error == OTA_ERR_CRC); confirm();
    /* 确认版本已坏且无上一个槽，必须立即进入稳定源，不猜测另一个槽的内容。 */
    flash[OTA_A1_BASE - FLASH_BASE + 100U] ^= 1U;
    CHECK(restart() == 1 && jumped == OTA_A1_BASE && loaded().stage == 2U);
    message("PASS: C boot manager install, restart, confirmation, 3+3, stable and FRU\n"); finish(0U);
}
