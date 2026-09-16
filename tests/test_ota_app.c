/* 直接执行App/ota/ota_app.c；QSPI以RAM模拟，只验证下载事务和健康门，不代表板级时序。 */
#include "ota_app.h"
#include "boot_record.h"
#include "ota_test_port.h"
#include "flash_layout.h"
#include "pc_ota.h"
#include "pc_command.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

#define PACKAGE ((const uint8_t *)0x60000000U)
#define CONFIG (*(const uint32_t *)0x60400000U)
#define S1(x) #x
#define S(x) S1(x)
#define CHECK(c) do { if (!(c)) die("FAIL " S(__LINE__) ": " #c "\n"); } while (0)
static uint8_t nor[0x210000];
static uint32_t now, locked, maintenance, power_on, bad_io, bad_commit;
static uint32_t erases, writes, resets;
ota_test_scb_t ota_test_scb;

/* QEMU专用有界堆供真实cJSON/newlib使用；与生产FreeRTOS分配器无关。 */
void *_sbrk(ptrdiff_t increment)
{
    static uint64_t heap[8192];
    static ptrdiff_t used;
    if (increment < -used || increment > (ptrdiff_t)sizeof(heap) - used) return (void *)-1;
    void *result = (uint8_t *)heap + used; used += increment; return result;
}

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
uint32_t HAL_GetTick(void) { return now; }
void NVIC_SystemReset(void) { ++resets; }
int flash_transaction_begin(uint32_t timeout)
{
    CHECK(timeout <= 1000U && !locked); locked = 1U; return 0;
}
void flash_transaction_end(void) { CHECK(locked); locked = 0U; }
int monitor_get_latest(monitor_snapshot_t *s)
{
    s->timestamp_ms = now; s->power_state = power_on; return 1;
}
int pwr_ctrl_get_state(void) { return (int)power_on; }
int monitor_ota_enter(void)
{
    if (maintenance || power_on) return -1;
    maintenance = 1U; return 0;
}
void monitor_ota_leave(void) { CHECK(maintenance); maintenance = 0U; }
int qspi_read_data(uint32_t address, uint8_t *data, uint32_t size)
{
    CHECK(locked && address + size <= sizeof(nor));
    memcpy(data, nor + address, size); return 0;
}
int qspi_write_page(uint32_t address, const uint8_t *data, uint16_t size)
{
    CHECK(locked && size && size <= 256U - (address & 255U) && address + size <= sizeof(nor));
    CHECK(address >= OTA_DOWNLOAD_BASE || (address >= 0x4000U && address + size <= 0x6000U));
    if (address >= OTA_DOWNLOAD_BASE) CHECK(maintenance && !power_on);
    ++writes;
    for (uint32_t i = 0; i < size; ++i) {
        CHECK((nor[address + i] & data[i]) == data[i]); nor[address + i] &= data[i];
        if (bad_io && i == size / 2U) return -1;
    }
    /* 提交字确实已落盘但底层报错，App也必须锁住后续测试/下载，等待B裁决。 */
    if (bad_commit && address < 0x6000U && (address & 255U) == 252U) return -1;
    return 0;
}
int qspi_erase_sector(uint32_t address)
{
    CHECK(locked && !(address & 4095U) && address + 4096U <= sizeof(nor));
    CHECK(address >= OTA_DOWNLOAD_BASE || address == 0x4000U || address == 0x5000U);
    if (address >= OTA_DOWNLOAD_BASE) CHECK(maintenance && !power_on);
    memset(nor + address, 255, 4096); ++erases; return 0;
}
static int read_record(void *c, uint32_t a, uint8_t *d, uint32_t n) { (void)c; return qspi_read_data(a,d,n); }
static int write_record(void *c, uint32_t a, const uint8_t *d, uint32_t n)
{ (void)c; CHECK(n <= 256U); return qspi_write_page(a,d,(uint16_t)n); }
static int erase_record(void *c, uint32_t a) { (void)c; return qspi_erase_sector(a); }
static const boot_store_t store = {NULL, read_record, write_record, erase_record};
static int version_read(flash_if_t *f, uint32_t a, uint8_t *d, uint32_t n)
{ (void)f; return qspi_read_data(a, d, n); }
static int version_write(flash_if_t *f, uint32_t a, const uint8_t *d, uint32_t n)
{ (void)f; CHECK(locked && a + n <= 0x2000U); memcpy(nor + a, d, n); return 0; }
flash_if_t flash_if = {.read = version_read, .write = version_write};
static boot_record_t loaded(void)
{
    boot_record_t r; flash_transaction_begin(1000);
    CHECK(boot_record_load(&store, &r) == 0); flash_transaction_end(); return r;
}
static void progress(void)
{
    for (uint32_t i = 0; i < 5U; ++i) ota_app_progress(i);
}

/** 实际cJSON和PC OTA处理函数，校验返回命令/流水号及JSON；不伪造协议实现。 */
static cJSON *request(const char *json, uint16_t expected_command)
{
    cJSON *root = NULL, *body = NULL;
    pc_dispatch_response_t response;
    pc_protocol_frame_t frame = {.command_id = PC_COMMAND_MCU_OTA_REQUEST,
        .json = (const uint8_t *)json, .json_length = (uint32_t)strlen(json), .sequence = 999U};
    CHECK(pc_json_parse_object(frame.json, frame.json_length, &root, NULL) == PC_JSON_OK);
    CHECK(pc_ota_handle(&frame, 77U, root, &response) == PC_DISPATCH_RESPONSE_READY);
    cJSON_Delete(root);
    CHECK(response.length >= 18U && response.data[10] == expected_command >> 8U &&
          response.data[11] == (expected_command & 255U) && response.data[15] == 77U);
    CHECK(pc_json_parse_object(response.data + 18, response.length - 18U, &body, NULL) == PC_JSON_OK);
    return body;
}
static void expect_error(const char *json, const char *error)
{
    cJSON *body = request(json, 6U);
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(body, "errorCode")->valuestring, error) == 0);
    cJSON_Delete(body);
}
static void protocol_checks(uint32_t id, uint32_t length)
{
    char json[1800];
    cJSON *body = request("{\"action\":\"query\",\"packageId\":\"11223344\"}", 0x0903U);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "confirmed"))); cJSON_Delete(body);
    expect_error("{\"action\":\"x\"}", "notSupported");
    expect_error("{\"action\":\"query\",\"unknown\":1}", "invalidArgument");
    expect_error("{\"action\":\"query\",\"packageId\":\"aabbccdd\"}", "invalidArgument");
    expect_error("{\"action\":\"begin\",\"packageId\":\"00000000\",\"packageSize\":true}", "invalidArgument");
    expect_error("{\"action\":\"begin\",\"packageId\":\"00000000\",\"packageSize\":116.5}", "invalidArgument");
    const char *bad[] = {"", "Zg", "Zh==", "Zm9=", "Zg==AAAA", "====", "AA-A", "AA_A", "AA A"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        (void)snprintf(json, sizeof(json), "{\"action\":\"write\",\"packageId\":\"%08lX\",\"offset\":0,\"data\":\"%s\"}", (unsigned long)id, bad[i]);
        expect_error(json, "invalidArgument");
    }
    (void)snprintf(json, sizeof(json), "{\"action\":\"begin\",\"packageId\":\"%08lX\",\"packageSize\":%lu}", (unsigned long)id, (unsigned long)length);
    cJSON_Delete(request(json, 0x0902U));
    cJSON_Delete(request(json, 0x0902U)); /* begin重试不能擦写。 */
    uint32_t before = writes;
    (void)snprintf(json, sizeof(json), "{\"action\":\"write\",\"packageId\":\"%08lX\",\"offset\":0,\"data\":\"T1RBMQ==\"}", (unsigned long)id);
    cJSON_Delete(request(json, 0x0902U)); CHECK(writes == before + 1U);
    cJSON_Delete(request(json, 0x0902U)); CHECK(writes == before + 1U);
    CHECK(ota_download_write(id, 0U, (const uint8_t *)"diff", 4U) == OTA_DOWNLOAD_DATA);
    CHECK(ota_download_write(id, 3U, PACKAGE, 4U) == OTA_DOWNLOAD_OFFSET);
    CHECK(ota_download_begin(id, length + 1U) == OTA_DOWNLOAD_ARGUMENT);
    CHECK(ota_download_begin(id ^ 1U, length) == OTA_DOWNLOAD_BUSY);
    ota_download_progress_t p; ota_download_get_progress(&p); CHECK(p.received_bytes == 4U);
    body = request("{\"action\":\"query\"}", 0x0902U);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "confirmed")) &&
          cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(body, "download"))); cJSON_Delete(body);
    now += 29999U; ota_download_poll(); CHECK(maintenance);
    cJSON_Delete(request("{\"action\":\"query\"}", 0x0902U));
    CHECK(ota_download_begin(id, length) == 0);
    ++now; ota_download_poll(); CHECK(!maintenance);
    CHECK(ota_download_cancel(id) == 0); /* 同连接超时取消也可重复确认取消。 */
    ota_download_disconnected();
    CHECK(ota_download_cancel(id) == OTA_DOWNLOAD_STATE);
    CHECK(ota_download_begin(id, length) == 0);
    ota_download_disconnected(); CHECK(!maintenance && ota_app_is_confirmed());
    /* 包ID为0合法，不能当作“没有ID”；这里只开启/取消，不伪造CRC为0的文件。 */
    CHECK(ota_download_begin(0U, length) == 0 && ota_download_cancel(0U) == 0);
    /* 前116字节即拒绝错误包，不必等整个下载结束；每次失败释放维护门。 */
    uint8_t prefix[OTA_PREFIX_BYTES];
    memcpy(prefix, PACKAGE, sizeof(prefix)); prefix[0] ^= 1U;
    CHECK(ota_download_begin(id, length) == 0);
    CHECK(ota_download_write(id, 0, prefix, sizeof(prefix)) == OTA_DOWNLOAD_PACKAGE && !maintenance);
    CHECK(ota_download_begin(id ^ 1U, length) == 0);
    CHECK(ota_download_write(id ^ 1U, 0, PACKAGE, sizeof(prefix)) == OTA_DOWNLOAD_CRC && !maintenance);
    for (uint32_t kind = 0; kind < 2U; ++kind) {
        memcpy(prefix, PACKAGE, sizeof(prefix));
        if (kind == 0U) prefix[100] = 'X';
        else memset(prefix + 44, 0, 6); /* 当前0.0.0，普通更新不得同版本/降版。 */
        uint32_t crc = ota_crc32(0, prefix + 40, 76); memcpy(prefix + 28, &crc, 4);
        crc = ota_crc32(0, prefix, 36); memcpy(prefix + 36, &crc, 4);
        CHECK(ota_download_begin(crc, length) == 0);
        CHECK(ota_download_write(crc, 0, prefix, sizeof(prefix)) ==
              (kind == 0U ? OTA_DOWNLOAD_TARGET : OTA_DOWNLOAD_VERSION));
        CHECK(!maintenance);
    }
    /* 版本区虽有合法CRC，但与实际活动槽不符时，查询也必须明确失败。 */
    firmware_version_info_t version;
    flash_transaction_begin(1000); CHECK(firmware_version_load(&flash_if, &version, NULL) == 0);
    version.version_patch++; CHECK(firmware_version_save(&flash_if, &version, NULL) == 0); flash_transaction_end();
    expect_error("{\"action\":\"query\"}", "flashError");
    flash_transaction_begin(1000); version.version_patch--;
    CHECK(firmware_version_save(&flash_if, &version, NULL) == 0); flash_transaction_end();
    progress();
}
void test_main(void)
{
    uint32_t length, id; memcpy(&length, PACKAGE + 12, 4); memcpy(&id, PACKAGE + 36, 4);
    memset(nor, 255, sizeof(nor)); memset(nor + 0x2000, 0x3C, 8192);
    boot_record_t r = {0}; r.previous = BOOT_SLOT_NONE; r.valid_mask = 1U; r.pending = 1U; r.epoch = 7U;
    ota_image_t *im = &r.images[0]; im->slot_base = OTA_A1_BASE; im->image_bytes = 128;
    im->version_payload[0] = 1; im->version_payload[2] = 76; im->version_payload[12] = 128;
    /* 测试App与包的板型须显式一致，不从待测包反向猜测可信板型。 */
    _Static_assert(sizeof(OTA_TEST_TARGET) <= 16U, "Target field overflow");
    memcpy(im->version_payload + 60, OTA_TEST_TARGET, sizeof(OTA_TEST_TARGET));
    im->package_header_crc32 = 0x11223344U;
    flash_transaction_begin(1000); CHECK(boot_record_save(&store, &r) == 0);
    firmware_version_info_t version; memcpy(&version, im->version_payload, sizeof(version));
    CHECK(firmware_version_save(&flash_if, &version, NULL) == 0); flash_transaction_end();
    ota_test_scb.VTOR = OTA_A1_BASE; ota_app_prepare();
    CHECK(!ota_app_is_confirmed() && ota_download_begin(id, length) != 0);
    for (now = 0; now <= 30000; now += 500) { progress(); CHECK(ota_app_watchdog_tick() == 1); }
    CHECK(ota_app_is_confirmed()); r = loaded(); CHECK(r.pending == 0 && r.phase == BOOT_NORMAL);
    protocol_checks(id, length);
    CHECK(ota_download_begin(id, 0) != 0 && ota_download_begin(id, OTA_PACKAGE_LIMIT + 1U) != 0);
    power_on = 1; CHECK(ota_download_begin(id, length) != 0); power_on = 0;
    uint32_t old_erases = erases;
    CHECK(ota_download_begin(id, length) == 0 && maintenance && erases == old_erases);
    CHECK(!ota_app_is_confirmed() && ota_download_begin(id, length) == 0);
    CHECK(ota_download_write(id, 1, PACKAGE, 1) != 0 && ota_download_write(id, 0, NULL, 1) != 0);
    CHECK(ota_download_write(id, 0, PACKAGE, 1025) != 0 && ota_download_finish(id) != 0);
    CHECK(ota_download_cancel(id) == 0); CHECK(!maintenance && ota_app_is_confirmed());
    CHECK(ota_download_begin(id, length) == 0);
    if (CONFIG == 1U) bad_io = 1U;
    for (uint32_t offset = 0; offset < length;) {
        uint32_t n = length - offset; if (n > 113U) n = 113U; /* 非页对齐的网络分块。 */
        int result = ota_download_write(id, offset, PACKAGE + offset, n);
        if (CONFIG == 1U) {
            CHECK(result != 0 && !maintenance && ota_app_is_confirmed());
            CHECK(loaded().phase == BOOT_NORMAL); goto done;
        }
        CHECK(result == 0); offset += n; now += 1; progress();
    }
    CHECK(memcmp(nor + OTA_DOWNLOAD_BASE, PACKAGE, length) == 0);
    if (CONFIG == 2U) nor[OTA_DOWNLOAD_BASE + length - 1U] ^= 1U;
    if (CONFIG == 3U) bad_commit = 1U;
    int result = ota_download_finish(id);
    if (CONFIG == 2U) {
        CHECK(result != 0 && !maintenance && ota_app_is_confirmed());
        CHECK(loaded().phase == BOOT_NORMAL);
    } else {
        CHECK((result == 0) == (CONFIG == 0U));
        CHECK(maintenance && !ota_app_is_confirmed() && loaded().phase == BOOT_READY);
        CHECK(ota_download_cancel(id) != 0); CHECK(maintenance && ota_download_begin(id, length) != 0);
        uint32_t before = writes;
        CHECK(ota_download_finish(id) == result && writes == before);
        ota_download_disconnected(); CHECK(maintenance);
        now += 4000U; progress(); CHECK(ota_app_watchdog_tick() == 0 && resets == 1U);
    }
done:
    CHECK(!locked && writes);
    for (uint32_t i = 0x2000; i < 0x4000; ++i) CHECK(nor[i] == 0x3CU);
    if (CONFIG == 1U || CONFIG == 2U) {
        now += 6000; CHECK(ota_app_watchdog_tick() == 0); progress(); CHECK(ota_app_watchdog_tick() == 0);
    }
    message("PASS: actual App health, download transaction and FRU\n"); finish(0U);
}
