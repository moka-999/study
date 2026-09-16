/* 真实C日志的NOR断电模拟：每字节提交中断、扇区回收、回绕和3+3恢复。 */
#include "boot_record.h"
#include "boot_health.h"
#include <string.h>

#define STR1(x) #x
#define STR(x) STR1(x)
#define CHECK(c) do { ++checks; if (!(c)) die("FAIL " STR(__LINE__) ": " #c "\n"); } while (0)
static uint8_t nor[8192], snapshot[8192];
static int budget, erase_budget, read_error;
static unsigned checks;

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
static _Noreturn void die(const char *why) { message(why); finish(1U); }
void test_abort(void) { die("FAIL CPU fault\n"); }

static int read_nor(void *c, uint32_t address, uint8_t *data, uint32_t bytes)
{
    (void)c;
    CHECK(address >= BOOT_JOURNAL_BASE && address - BOOT_JOURNAL_BASE + bytes <= sizeof(nor));
    if (read_error) return -1;
    memcpy(data, nor + address - BOOT_JOURNAL_BASE, bytes);
    return 0;
}
static int program(void *c, uint32_t address, const uint8_t *data, uint32_t bytes)
{
    (void)c;
    CHECK(address >= BOOT_JOURNAL_BASE && address - BOOT_JOURNAL_BASE + bytes <= sizeof(nor));
    for (uint32_t i = 0; i < bytes; ++i) {
        if (budget == 0) return -1;
        --budget;
        uint8_t *p = nor + address - BOOT_JOURNAL_BASE + i;
        CHECK((*p & data[i]) == data[i]);
        *p &= data[i];
    }
    return 0;
}
static int erase(void *c, uint32_t address)
{
    (void)c;
    CHECK(address == BOOT_JOURNAL_BASE || address == BOOT_JOURNAL_BASE + 4096U);
    for (uint32_t i = 0; i < 4096U; ++i) {
        if (erase_budget == 0) return -1;
        --erase_budget;
        nor[address - BOOT_JOURNAL_BASE + i] = 255U;
    }
    return 0;
}
static const boot_store_t store = {NULL, read_nor, program, erase};

static boot_record_t factory(void)
{
    boot_record_t r = {0};
    r.previous = 1U; r.valid_mask = 3U;
    for (uint32_t i = 0; i < 2; ++i) {
        r.images[i].slot_base = boot_slot_base(i);
        r.images[i].image_bytes = 128U;
        r.images[i].version_payload[0] = 1U;
        r.images[i].version_payload[2] = 76U;
        r.images[i].version_payload[12] = 128U;
    }
    return r;
}

void test_main(void)
{
    boot_record_t r, loaded;
    memset(nor, 255, sizeof(nor)); budget = erase_budget = 1000000;
    CHECK(boot_record_load(&store, &r) == BOOT_STORE_EMPTY);
    r = factory(); CHECK(boot_record_save(&store, &r) == 0 && r.generation == 1U);
    boot_record_t stale = r;
    r.last_error = 17U; CHECK(boot_record_save(&store, &r) == 0);
    CHECK(boot_record_save(&store, &stale) == BOOT_STORE_STALE);
    /* 所有正文/CRC/最终提交字节边界；失败不能让旧记录失效。 */
    memcpy(snapshot, nor, sizeof(nor));
    for (int cut = 0; cut <= 256; ++cut) {
        memcpy(nor, snapshot, sizeof(nor)); budget = cut;
        boot_record_t changed = r; changed.last_error = 99U;
        int result = boot_record_save(&store, &changed);
        CHECK((result == 0) == (cut == 256));
        CHECK(boot_record_load(&store, &loaded) == 0);
        CHECK(loaded.generation == r.generation + (cut == 256 ? 1U : 0U));
        CHECK(loaded.last_error == (cut == 256 ? 99U : 17U));
        /* 掉电重进后跳过残缺尾页，仍能追加完整事务。 */
        budget = 1000000; loaded.last_error = 42U;
        CHECK(boot_record_save(&store, &loaded) == 0);
    }
    memcpy(nor, snapshot, sizeof(nor)); budget = erase_budget = 1000000;
    CHECK(boot_record_load(&store, &r) == 0);
    for (unsigned i = 0; i < 30; ++i) CHECK(boot_record_save(&store, &r) == 0);
    CHECK(r.generation == 32U); /* 最新记录位于第二扇区末尾，即将回收第一扇区。 */
    memcpy(snapshot, nor, sizeof(nor));
    const int cuts[] = {0, 1, 255, 256, 2048, 4095, 4096};
    for (unsigned i = 0; i < sizeof(cuts) / sizeof(cuts[0]); ++i) {
        memcpy(nor, snapshot, sizeof(nor)); erase_budget = cuts[i]; budget = 1000000;
        boot_record_t changed = r;
        CHECK((boot_record_save(&store, &changed) == 0) == (cuts[i] == 4096));
        CHECK(boot_record_load(&store, &loaded) == 0);
        CHECK(loaded.generation == (cuts[i] == 4096 ? 33U : 32U));
    }
    /* 回收成功但新页任意字节掉电，唯一旧有效扇区仍保留。 */
    for (int cut = 0; cut < 256; ++cut) {
        memcpy(nor, snapshot, sizeof(nor)); budget = cut; erase_budget = 1000000;
        boot_record_t changed = r;
        CHECK(boot_record_save(&store, &changed) != 0);
        CHECK(boot_record_load(&store, &loaded) == 0 && loaded.generation == 32U);
    }
    read_error = 1; CHECK(boot_record_load(&store, &loaded) == BOOT_STORE_IO); read_error = 0;
    memset(nor, 0, sizeof(nor)); CHECK(boot_record_load(&store, &loaded) == BOOT_STORE_INVALID);
    /* 白盒构造接近回绕的旧记录，CRC仍合法；实际save必须能跨UINT32_MAX。 */
    memcpy(nor, snapshot, sizeof(nor));
    memmove(nor, nor + 31U * 256U, 256U); memset(nor + 256, 255, sizeof(nor) - 256);
    memset(nor + 8, 255, 4);
    uint32_t crc = ota_crc32(0, nor, 248U); memcpy(nor + 248, &crc, 4);
    CHECK(boot_record_load(&store, &r) == 0 && r.generation == UINT32_MAX);
    budget = erase_budget = 1000000;
    CHECK(boot_record_save(&store, &r) == 0 && r.generation == 0U);
    CHECK(boot_record_load(&store, &loaded) == 0 && loaded.generation == 0U);
    memcpy(nor + 512, nor + 256, 256); CHECK(boot_record_load(&store, &loaded) == BOOT_STORE_INVALID);
    /* 当前三次未确认→上个槽；再三次→稳定区；稳定版再三次→维护。 */
    r = factory();
    for (unsigned i = 1; i <= 6; ++i) {
        r.pending = 1U; boot_record_failed_start(&r);
        CHECK(r.stage == (i < 3 ? 0U : i < 6 ? 1U : 2U));
        if (i == 3) CHECK(r.active == 1U && r.valid_mask == 2U);
    }
    CHECK(r.phase == BOOT_INSTALLING && r.valid_mask == 0U);
    r.active = 0U; r.phase = BOOT_TRIAL; r.valid_mask = 1U;
    for (unsigned i = 0; i < 3; ++i) { r.pending = 1U; boot_record_failed_start(&r); }
    CHECK(r.phase == BOOT_MAINTENANCE && r.stage == 3U);
    r = factory(); r.pending = 1U; r.epoch = 12U; r.failures = 2U;
    CHECK(boot_record_confirm(&r, OTA_A2_BASE, 12U) != 0);
    CHECK(boot_record_confirm(&r, OTA_A1_BASE, 11U) != 0);
    CHECK(boot_record_confirm(&r, OTA_A1_BASE, 12U) == 0 && r.failures == 0U && r.pending == 0U);
    boot_record_failed_start(&r); CHECK(r.failures == 0U);
    boot_health_t health;
    boot_health_init(&health, 31U, 0U);
    CHECK(boot_health_poll(&health, 24999U) == 1);
    CHECK(boot_health_poll(&health, 25000U) == 0); /* 缺必要任务不得永久无条件喂狗。 */
    boot_health_init(&health, 31U, UINT32_MAX - 5000U);
    for (uint32_t elapsed = 0U; elapsed <= 30000U; elapsed += 1000U) {
        uint32_t now = UINT32_MAX - 5000U + elapsed;
        for (uint32_t id = 0U; id < 5U; ++id) boot_health_progress(&health, id, now);
        CHECK(boot_health_poll(&health, now) == (elapsed == 30000U ? 2 : 1));
    }
    CHECK(boot_health_poll(&health, UINT32_MAX - 5000U + 35001U) == 0);
    message("PASS: C boot journal power cuts and 3+3 recovery\n");
    finish(0U);
}
