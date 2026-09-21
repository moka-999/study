/* 运行正式monitor/MCP4725代码；仅桩化锁、OTA门和I2C，不模拟真实电气状态。 */
#include "monitor_task.h"
#include "boot_test_port.h"
#include <stddef.h>

#define S1(x) #x
#define S(x) S1(x)
#define CHECK(c) do { if (!(c)) fail("FAIL " S(__LINE__) ": " #c "\n"); } while (0)
static uint32_t writes;
static int locked, busy, io_error, confirmed = 1;
static pwr_ctrl_state_t state = PWR_CTRL_STATE_OFF;

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

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { return storage; }
int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    (void)timeout;
    CHECK(semaphore != NULL && !locked);
    if (busy) return 0;
    locked = 1;
    return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    CHECK(semaphore != NULL && locked);
    locked = 0;
    return pdTRUE;
}
int ota_app_is_confirmed(void) { return confirmed; }
pwr_ctrl_state_t pwr_ctrl_get_state(void) { return state; }
uint32_t HAL_GetTick(void) { return 0U; }
pwr_ctrl_result_t pwr_ctrl_request_off(uint32_t now)
{
    (void)now;
    state = PWR_CTRL_STATE_OFF;
    return PWR_CTRL_OK;
}

static int write_dac(i2c_if_t *bus, uint8_t address,
                     const uint8_t *bytes, uint16_t count)
{
    CHECK(locked && bus != NULL && address == MCP4725_ADDR_A0_LOW);
    /* emmc=600mV，对应Fast Write 02 E9；不允许EEPROM命令或额外写。 */
    CHECK(count == 2U && bytes[0] == 0x02U && bytes[1] == 0xE9U);
    ++writes;
    return io_error ? IF_ERR : IF_OK;
}
i2c_if_t i2c2_if = {.write_bytes = write_dac};

static void expect_cached(void)
{
    boot_mode_t mode = BOOT_MODE_COUNT;
    uint16_t mv = 9999U;
    CHECK(monitor_get_boot_mode(&mode, &mv) == MONITOR_BOOT_OK);
    CHECK(mode == BOOT_MODE_EMMC && mv == 600U && !locked);
}

static void expect_unknown(void)
{
    boot_mode_t mode = BOOT_MODE_COUNT;
    uint16_t mv = 9999U;
    CHECK(monitor_get_boot_mode(&mode, &mv) == MONITOR_BOOT_ERR_NOT_SET);
    CHECK(mode == BOOT_MODE_COUNT && mv == 9999U && !locked);
}

void test_main(void)
{
    /* MCU冷启动BSS为空，准备句柄不能自动写DAC。 */
    CHECK(monitor_task_prepare() == 1 && writes == 0U);
    expect_unknown();
    CHECK(monitor_set_boot_mode(BOOT_MODE_EMMC) == MONITOR_BOOT_OK);
    expect_cached();

    /* 未访问器件的拒绝必须保留旧配置，且不得额外发送I2C。 */
    CHECK(monitor_set_boot_mode(BOOT_MODE_COUNT) == MONITOR_BOOT_ERR_PARAM);
    expect_cached();
    busy = 1;
    CHECK(monitor_set_boot_mode(BOOT_MODE_EMMC) == MONITOR_BOOT_ERR_BUSY);
    busy = 0;
    expect_cached();
    state = PWR_CTRL_STATE_ON;
    CHECK(monitor_set_boot_mode(BOOT_MODE_EMMC) == MONITOR_BOOT_ERR_STATE);
    expect_cached();
    CHECK(monitor_request_power_off(0U) == PWR_CTRL_OK);
    expect_cached();
    confirmed = 0;
    CHECK(monitor_set_boot_mode(BOOT_MODE_EMMC) == MONITOR_BOOT_ERR_STATE);
    confirmed = 1;
    expect_cached();
    CHECK(monitor_ota_enter() == 0);
    CHECK(monitor_set_boot_mode(BOOT_MODE_EMMC) == MONITOR_BOOT_ERR_STATE);
    monitor_ota_leave();
    expect_cached();
    CHECK(writes == 1U);

    /* 已开始的I2C可能部分完成：报错后必须拒绝把上次模式当作当前配置。 */
    io_error = 1;
    CHECK(monitor_set_boot_mode(BOOT_MODE_EMMC) == MONITOR_BOOT_ERR_IO);
    CHECK(writes == 2U);
    expect_unknown();
    io_error = 0;
    CHECK(monitor_set_boot_mode(BOOT_MODE_EMMC) == MONITOR_BOOT_OK);
    expect_cached();
    CHECK(writes == 3U && !locked);
    message("PASS: actual monitor Boot cache failure and rejection paths\n");
    finish(0U);
}
