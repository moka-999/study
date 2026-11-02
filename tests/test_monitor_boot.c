/* 运行正式monitor/统计/MCP4725代码；器件和RTOS使用桩，不模拟真实电气状态。 */
#include "monitor_task.h"
#include "task_watchdog.h"
#include "boot_test_port.h"
#include <stddef.h>
#include <setjmp.h>
#include <string.h>

#define S1(x) #x
#define S(x) S1(x)
#define CHECK(c) do { if (!(c)) fail("FAIL " S(__LINE__) ": " #c "\n"); } while (0)
static uint32_t writes;
static int locked, busy, io_error;
static pwr_ctrl_state_t state = PWR_CTRL_STATE_OFF;
static uint32_t clock_ms, samples, loops, task_window_ms;
static uint32_t voltage_polls, voltage_samples, status_reads, init_calls;
static uint32_t last_init_ms, first_current_ms, last_current_ms, maximum_current_gap_ms;
static uint32_t voltage_begin_ms, last_voltage_begin_ms;
static uint32_t loop_limit;
static uint8_t voltage_collecting;
typedef enum {
    TASK_FAST,
    TASK_CROSS_WINDOW,
    TASK_CANCEL_DURING_READ,
    TASK_OFF,
    TASK_LOCK_BUSY,
    TASK_INIT_BACKOFF,
    TASK_CACHE_EXPIRE
} task_case_t;
static task_case_t task_case;
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
    memset(monitor, 0, sizeof(*monitor));
}
int memory_monitor_init(memory_monitor_t *monitor)
{
    CHECK(locked && monitor != NULL);
    if (init_calls != 0U) CHECK((uint32_t)(clock_ms - last_init_ms) >= 1000U);
    last_init_ms = clock_ms;
    ++init_calls;
    if (task_case == TASK_INIT_BACKOFF) return IF_ERR;
    monitor->pac_ready = 1U;
    monitor->adc_ready = 1U;
#ifdef MONITOR_TEST_B1
    monitor->adc.id = 0x04U;
    monitor->adc.last_error = 0x123U;
    monitor->pac.last_error = PAC1934_ERROR_NONE;
#endif
    return IF_OK;
}
uint8_t memory_monitor_ready_groups(const memory_monitor_t *monitor)
{
    CHECK(locked && monitor);
    return (monitor->adc_ready ? MEMORY_MONITOR_VALID_VOLTAGE : 0U) |
           (monitor->pac_ready ? MEMORY_MONITOR_VALID_IDD : 0U);
}
int memory_monitor_read_current(memory_monitor_t *monitor, memory_monitor_sample_t *sample)
{
    CHECK(locked && monitor && sample);
    CHECK(state == PWR_CTRL_STATE_ON);
    if (!monitor->pac_ready) return IF_ERR;
    if (samples != 0U) {
        uint32_t gap = clock_ms - last_current_ms;
        if (gap > maximum_current_gap_ms) maximum_current_gap_ms = gap;
    } else {
        first_current_ms = clock_ms;
    }
    last_current_ms = clock_ms;
    ++samples;
    clock_ms += task_case == TASK_CROSS_WINDOW ? 20U : 1U;
    sample->valid_mask |= MEMORY_MONITOR_VALID_IDD;
    for (uint32_t i = 0; i < MEMORY_MONITOR_RAIL_COUNT; ++i) {
        /* 每次相差100µA；提前转换整数mA会让均值和峰值检查失败。 */
        sample->idd[i].current_ua = samples * 100U + 17U + i;
        sample->idd[i].current_ma = (uint32_t)(sample->idd[i].current_ua / 1000U);
    }
    if (task_case == TASK_CANCEL_DURING_READ) monitor_idd_cancel();
    return IF_OK;
}
int memory_monitor_poll_voltage(memory_monitor_t *monitor, memory_monitor_sample_t *sample,
                                uint32_t now_ms, uint32_t *begin_ms)
{
    CHECK(locked && monitor && sample && begin_ms && now_ms == clock_ms);
    CHECK(state == PWR_CTRL_STATE_ON);
    if (!monitor->adc_ready) return 0;
    ++voltage_polls;
    /* 持续没有新电压，但电流正常更新；任务必须独立淘汰旧电压。 */
    if (task_case == TASK_CACHE_EXPIRE && voltage_samples != 0U) return 0;
    if (!voltage_collecting) {
        voltage_collecting = 1U;
        voltage_begin_ms = now_ms;
    }
    *begin_ms = voltage_begin_ms;
    /* 两次尚未完成、第三次才形成完整三轨；不能重复统计缓存或把pending当失败。 */
    if (voltage_polls % 3U != 0U) return 0;
    voltage_collecting = 0U;
    last_voltage_begin_ms = voltage_begin_ms;
    ++voltage_samples;
    sample->valid_mask |= MEMORY_MONITOR_VALID_VOLTAGE;
    for (uint32_t i = 0; i < MEMORY_MONITOR_RAIL_COUNT; ++i)
        sample->voltage_mv[i] = 1000 + (int32_t)(voltage_samples * 10U + i);
    return 1;
}
int memory_monitor_read_status(memory_monitor_t *monitor, memory_monitor_sample_t *sample)
{
    CHECK(locked && monitor && sample && state == PWR_CTRL_STATE_ON);
    ++status_reads;
    sample->valid_mask |= MEMORY_MONITOR_VALID_TPS_STATUS;
    return IF_OK;
}
void memory_monitor_invalidate(memory_monitor_t *monitor)
{
    CHECK(locked && monitor);
    monitor->pac_ready = monitor->adc_ready = 0U;
}
void task_watchdog_progress(uint32_t task_id) { CHECK(!locked && task_id == TASK_WATCHDOG_MONITOR); }
void vTaskDelay(TickType_t ticks)
{
    CHECK(!locked);
    if (ticks == 1000U) {
        clock_ms += ticks;
        task_window_ms = task_case == TASK_CROSS_WINDOW ? 10U : 10000U;
        if (state == PWR_CTRL_STATE_ON)
            CHECK(monitor_idd_start(task_window_ms, 1U) == IF_OK);
        if (task_case == TASK_LOCK_BUSY) busy = 1;
    } else {
        CHECK(ticks == (state == PWR_CTRL_STATE_OFF || busy ? 20U : 1U));
        monitor_snapshot_t snapshot;
        CHECK(monitor_get_latest(&snapshot));
        if (task_case == TASK_FAST && voltage_samples != 0U) {
            /* 快速电流更新不能给尚未更新的电压缓存续命。 */
            CHECK(snapshot.timestamp_ms == last_voltage_begin_ms);
        }
        if (++loops == loop_limit) longjmp(task_exit, 1);
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


    /* 无新样本且器件锁一直忙，也必须按期TIMEOUT，不等待下一次有效采样。 */
    state = PWR_CTRL_STATE_ON;
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

    /* 跑实际任务：电流不等AD完整扫描，另检查失败退避、旧缓存和统计边界。 */
    for (unsigned run = TASK_FAST; run <= TASK_CACHE_EXPIRE; ++run) {
        task_case = (task_case_t)run;
        state = task_case == TASK_OFF ? PWR_CTRL_STATE_OFF : PWR_CTRL_STATE_ON;
        busy = 0;
        samples = loops = 0U;
        voltage_polls = voltage_samples = status_reads = init_calls = 0U;
        maximum_current_gap_ms = 0U;
        voltage_collecting = 0U;
        loop_limit = task_case == TASK_INIT_BACKOFF ? 2200U :
                     task_case == TASK_CACHE_EXPIRE ? 1200U : 120U;
        monitor_idd_cancel();
        if (setjmp(task_exit) == 0) StartMonitorTask(NULL);
        CHECK(!locked);
        busy = 0;
        CHECK(monitor_idd_get(&stats));
        monitor_idd_finish();
        CHECK(monitor_idd_get(&stats));
        if (task_case == TASK_FAST) {
            CHECK(stats.state == IDD_STATS_COMPLETE && samples == loop_limit);
            CHECK(maximum_current_gap_ms <= 3U && voltage_samples > 1U);
            CHECK(samples > voltage_samples * 5U);
            CHECK(clock_ms - first_current_ms < 500U);
            for (uint32_t i = 0; i < MEMORY_MONITOR_RAIL_COUNT; ++i) {
                CHECK(stats.voltage[i].valid_count == voltage_samples);
                CHECK(stats.voltage[i].failed_count == 0U);
                CHECK(stats.current[i].valid_count == samples);
                CHECK(stats.current[i].failed_count == 0U);
                CHECK(stats.voltage[i].mean == 1000 + 5 * ((int64_t)voltage_samples + 1) + i);
                CHECK(stats.current[i].mean == 50 * ((int64_t)samples + 1) + 17 + i);
                CHECK(stats.current[i].maximum == (int64_t)samples * 100 + 17 + i);
            }
            CHECK(init_calls == 1U && status_reads == 1U);
        } else if (task_case == TASK_CROSS_WINDOW || task_case == TASK_CANCEL_DURING_READ) {
            CHECK(stats.attempts == 0U);
            CHECK(stats.state == (task_case == TASK_CROSS_WINDOW ? IDD_STATS_TIMEOUT : IDD_STATS_CANCELLED));
            CHECK(task_case != TASK_CROSS_WINDOW || stats.discarded > 0U);
        } else if (task_case == TASK_OFF || task_case == TASK_LOCK_BUSY) {
            CHECK(samples == 0U && voltage_polls == 0U && init_calls == 0U && status_reads == 0U);
        } else if (task_case == TASK_INIT_BACKOFF) {
            CHECK(init_calls >= 2U && init_calls <= 3U && samples == 0U);
        } else if (task_case == TASK_CACHE_EXPIRE) {
            monitor_snapshot_t expired;
            CHECK(samples > 0U && voltage_samples > 0U);
            CHECK(monitor_get_latest(&expired));
            CHECK((expired.memory.valid_mask & MEMORY_MONITOR_VALID_VOLTAGE) == 0U);
            CHECK((expired.memory.valid_mask & MEMORY_MONITOR_VALID_IDD) != 0U);
            /* 任务整体停止推进后，查询也不得继续发布过期的最后一份电流。 */
            clock_ms += 1001U;
            CHECK(monitor_get_latest(&expired) && expired.memory.valid_mask == 0U);
        }
        /* 最新快照仍存在，但新统计必须从空窗口开始，不能回灌旧数据。 */
#ifdef MONITOR_TEST_B1
        /* 诊断查询必须不取器件事务锁、不增加初始化/采集次数。 */
        monitor_diagnostics_t diagnostics;
        uint32_t previous_samples = samples, previous_init = init_calls;
        busy = 1;
        CHECK(!monitor_get_diagnostics(NULL));
        CHECK(monitor_get_diagnostics(&diagnostics) && !locked);
        CHECK(samples == previous_samples && init_calls == previous_init);
        if (task_case == TASK_OFF) CHECK(!diagnostics.adc_ready && !diagnostics.pac_ready);
        if (task_case == TASK_FAST) {
            CHECK(diagnostics.adc_ready && diagnostics.pac_ready);
            CHECK(diagnostics.adc_id == 0x04U && diagnostics.adc_last_error == 0x123U);
        }
        busy = 0;
#endif
        monitor_snapshot_t snapshot;
        CHECK(monitor_get_latest(&snapshot));
        state = PWR_CTRL_STATE_ON;
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
