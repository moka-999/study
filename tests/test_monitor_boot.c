/* 运行正式monitor/统计/MCP4725代码；器件和RTOS使用桩，不模拟真实电气状态。 */
#include "monitor_task.h"
#include "ota_app.h"
#include "boot_test_port.h"
#include <stddef.h>
#include <setjmp.h>
#include <string.h>

#define S1(x) #x
#define S(x) S1(x)
#define CHECK(c) do { if (!(c)) fail("FAIL " S(__LINE__) ": " #c "\n"); } while (0)
static uint32_t writes;
static int locked, busy, io_error, confirmed = 1;
static pwr_ctrl_state_t state = PWR_CTRL_STATE_OFF;
static uint32_t clock_ms, samples, loops, task_window_ms;
static int short_window, cancel_during_read;
static jmp_buf task_exit;

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
uint32_t HAL_GetTick(void) { return clock_ms; }
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
i2c_if_t i2c4_if;
spi_if_t spi2_if, spi4_if;

/* 仅替换电源/器件动作，测试真实monitor是否正确持锁和取消旧条件窗口。 */
void pwr_ctrl_process(uint32_t now) { CHECK(locked && now == clock_ms); }
ddr_memory_type_t ddr_power_get_memory_type(void) { return DDR_MEMORY_LPDDR4X; }
int ddr_power_build_step_targets(ddr_memory_type_t type,
                                  const int16_t steps[DDR_RAIL_COUNT],
                                  uint16_t mv[DDR_RAIL_COUNT])
{
    if (!steps || type > DDR_MEMORY_LPDDR4X) return IF_ERR;
    memset(mv, 0, DDR_RAIL_COUNT * sizeof(*mv));
    return IF_OK;
}
pwr_ctrl_result_t pwr_ctrl_select_memory_type(ddr_memory_type_t type)
{
    CHECK(locked && type <= DDR_MEMORY_LPDDR4X);
    return PWR_CTRL_OK;
}
pwr_ctrl_result_t pwr_ctrl_apply_ddr_steps(const int16_t steps[DDR_RAIL_COUNT])
{
    CHECK(locked && steps != NULL);
    return PWR_CTRL_ERR_FAULT; /* 已尝试调压但失败，旧窗口也必须取消。 */
}

void memory_monitor_configure(memory_monitor_t *monitor, i2c_if_t *pac,
                              spi_if_t *adc, GPIO_TypeDef *port, uint16_t pin,
                              void (*wait_ms)(uint32_t))
{
    (void)port;
    CHECK(!locked && monitor && pac == &i2c4_if && wait_ms);
#ifdef MONITOR_TEST_B1
    CHECK(adc == &spi4_if && pin == B1_SPI4_NCS_PIN);
#else
    CHECK(adc == NULL && port == NULL && pin == 0U);
#endif
}
int memory_monitor_init(memory_monitor_t *monitor)
{
    CHECK(locked && monitor != NULL);
    return IF_OK;
}
int memory_monitor_sample(memory_monitor_t *monitor, memory_monitor_sample_t *sample)
{
    CHECK(locked && monitor && sample);
    ++samples;
    clock_ms += 20U; /* 整次器件读取耗时，不把跨窗口完成的快照算进去。 */
    memset(sample, 0, sizeof(*sample));
    sample->valid_mask = MEMORY_MONITOR_VALID_VOLTAGE | MEMORY_MONITOR_VALID_IDD;
    for (uint32_t i = 0; i < MEMORY_MONITOR_RAIL_COUNT; ++i) {
        sample->voltage_mv[i] = 1000 + (int32_t)(samples * 10U + i);
        sample->idd[i].current_ma = samples * 100U + i;
    }
    if (cancel_during_read) monitor_idd_cancel();
    return IF_OK;
}
void memory_monitor_invalidate(memory_monitor_t *monitor) { CHECK(locked && monitor); }
void ota_app_progress(uint32_t task_id) { CHECK(!locked && task_id == OTA_TASK_MONITOR); }
void vTaskDelay(TickType_t ticks)
{
    CHECK(!locked);
    if (ticks == 1000U) {
        clock_ms += ticks;
        task_window_ms = short_window ? 10U : 10000U;
        CHECK(monitor_idd_start(task_window_ms, 1U) == IF_OK);
    } else {
        CHECK(ticks == 2000U);
        if (++loops == 3U) longjmp(task_exit, 1);
        clock_ms += ticks;
    }
}

static void test_idd_integration(void)
{
    idd_stats_t stats;
    const int16_t steps[DDR_RAIL_COUNT] = {0, 0, 0};
    CHECK(monitor_idd_get(NULL) == 0);
    CHECK(monitor_idd_get(&stats) && stats.state == IDD_STATS_IDLE);
    CHECK(monitor_idd_start(0U, 1U) == IF_ERR);
    CHECK(monitor_idd_start(100U, 1U) == IF_ERR); /* OFF不可启动。 */
    state = PWR_CTRL_STATE_ON;
    busy = 1;
    CHECK(monitor_idd_start(100U, 1U) == IF_TIMEOUT);
    busy = 0;
    confirmed = 0;
    CHECK(monitor_idd_start(100U, 1U) == IF_ERR);
    confirmed = 1;
    CHECK(monitor_idd_start(100U, 1U) == IF_OK);
    CHECK(monitor_idd_start(200U, 1U) == IF_ERR);
    CHECK(monitor_apply_ddr_config(DDR_MEMORY_LPDDR4X, NULL) == PWR_CTRL_ERR_PARAM);
    CHECK(monitor_apply_ddr_config(DDR_MEMORY_LPDDR4, steps) == PWR_CTRL_ERR_BUSY);
    CHECK(monitor_idd_get(&stats) && stats.state == IDD_STATS_RUNNING && !stats.attempts);
    CHECK(monitor_apply_ddr_config(DDR_MEMORY_LPDDR4X, steps) == PWR_CTRL_ERR_FAULT);
    CHECK(monitor_idd_get(&stats) && stats.state == IDD_STATS_CANCELLED);
    CHECK(monitor_idd_start(100U, 1U) == IF_OK);
    CHECK(monitor_request_power_off(clock_ms) == PWR_CTRL_OK);
    CHECK(monitor_idd_get(&stats) && stats.state == IDD_STATS_CANCELLED);

    /* 即使故障改变电源状态后尚未轮到process，进入OTA也不能遗留窗口。 */
    state = PWR_CTRL_STATE_ON;
    CHECK(monitor_idd_start(100U, 1U) == IF_OK);
    state = PWR_CTRL_STATE_OFF;
    CHECK(monitor_ota_enter() == 0);
    CHECK(monitor_idd_get(&stats) && stats.state == IDD_STATS_CANCELLED);
    state = PWR_CTRL_STATE_ON;
    CHECK(monitor_idd_start(100U, 1U) == IF_ERR);
    monitor_ota_leave();

    /* 无新样本且器件锁一直忙，也必须按期TIMEOUT，不等待下一轮2秒采样。 */
    CHECK(monitor_idd_start(100U, 1U) == IF_OK);
    clock_ms += 100U;
    busy = 1;
    monitor_power_process(clock_ms);
    busy = 0;
    CHECK(monitor_idd_get(&stats) && stats.state == IDD_STATS_TIMEOUT);
    CHECK(monitor_idd_start(100U, 1U) == IF_OK);
    state = PWR_CTRL_STATE_FAULT;
    monitor_power_process(clock_ms);
    CHECK(monitor_idd_get(&stats) && stats.state == IDD_STATS_CANCELLED);

    /* 跑真实任务循环三次：正常窗口、跨末端慢读取、读取中主动取消。 */
    for (unsigned run = 0; run < 3U; ++run) {
        state = PWR_CTRL_STATE_ON;
        samples = loops = 0U;
        short_window = run == 1U;
        cancel_during_read = run == 2U;
        if (setjmp(task_exit) == 0) StartMonitorTask(NULL);
        CHECK(samples == 3U && !locked);
        CHECK(monitor_idd_get(&stats));
        if (clock_ms < stats.started_ms + task_window_ms)
            clock_ms = stats.started_ms + task_window_ms;
        CHECK(monitor_idd_get(&stats));
        if (run == 0U) {
            CHECK(stats.state == IDD_STATS_COMPLETE && stats.attempts == 3U);
            for (uint32_t i = 0; i < MEMORY_MONITOR_RAIL_COUNT; ++i) {
                CHECK(stats.voltage[i].mean == 1020 + (int64_t)i);
                CHECK(stats.current[i].mean == 200 + (int64_t)i);
                CHECK(stats.current[i].maximum == 300 + (int64_t)i);
            }
        } else {
            CHECK(stats.attempts == 0U);
            CHECK(stats.state == (short_window ? IDD_STATS_TIMEOUT : IDD_STATS_CANCELLED));
            CHECK(stats.discarded == (short_window ? 1U : 0U));
        }
        /* 最新快照仍存在，但新统计必须从空窗口开始，不能回灌旧数据。 */
        monitor_snapshot_t snapshot;
        CHECK(monitor_get_latest(&snapshot) && snapshot.memory.valid_mask);
        CHECK(monitor_idd_start(100U, 1U) == IF_OK);
        CHECK(monitor_idd_get(&stats) && stats.attempts == 0U);
        monitor_idd_finish();
        CHECK(monitor_idd_get(&stats) && stats.state == IDD_STATS_TIMEOUT);
    }
}

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
    test_idd_integration();
    message("PASS: actual monitor Boot cache and IDD task integration\n");
    finish(0U);
}
