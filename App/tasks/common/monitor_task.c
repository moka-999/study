/*
 * DDR电源监测任务
 *
 * 仅在pwr_ctrl进入ON后初始化PAC1934和AD7124并采集三路快照。
 * B1使用真实器件与板级时序初值，默认OFF，仅在电源ON时采样。
 * 快照留给新PC–MCU JSON业务层读取，不再向旧TLV TCP队列发文本。
 */
#include "monitor_task.h"
#include "device_wait.h"
#include "task_watchdog.h"
#include "board_interfaces.h"
#include "mcp4725.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "main.h"
#include <string.h>

static monitor_sample_observer_t s_sample_observer;

void monitor_set_sample_observer(monitor_sample_observer_t observer)
{
    taskENTER_CRITICAL();
    s_sample_observer = observer;
    taskEXIT_CRITICAL();
}

void monitor_publish_sample(const memory_monitor_sample_t *sample, uint8_t groups,
                             uint32_t begin_ms, uint32_t end_ms)
{
    taskENTER_CRITICAL();
    monitor_sample_observer_t observer = s_sample_observer;
    taskEXIT_CRITICAL();
    if (observer != NULL) observer(sample, groups, begin_ms, end_ms);
}

static memory_monitor_t s_monitor;
static monitor_snapshot_t s_latest;
static monitor_diagnostics_t s_diagnostics;
static uint8_t s_has_snapshot;
static StaticSemaphore_t s_memory_io_mutex_storage;
static SemaphoreHandle_t s_memory_io_mutex;
static mcp4725_t s_boot_dac;
static boot_mode_t s_boot_mode;
static uint8_t s_boot_mode_valid;
static idd_stats_t s_idd_stats; /* 统计独立于最新快照；只在短临界区访问，不含器件IO。 */

#define MEMORY_IO_LOCK_TIMEOUT_MS 500U
/* 以下是软件调度/失效期限，不是器件已验收的实际采样率。 */
#define MONITOR_ADC_POLL_MS       10U
#define MONITOR_STATUS_PERIOD_MS 1000U
#define MONITOR_INIT_RETRY_MS    1000U
#define MONITOR_SAMPLE_STALE_MS  1000U
#define MONITOR_IDLE_DELAY_MS    20U

static void monitor_wait_ms(uint32_t milliseconds)
{
    device_wait_ms(milliseconds);
}

static void publish_snapshot(const monitor_snapshot_t *snapshot)
{
    taskENTER_CRITICAL();
    s_latest = *snapshot;
    s_diagnostics.adc_ready = s_monitor.adc_ready;
    s_diagnostics.pac_ready = s_monitor.pac_ready;
    s_diagnostics.adc_id = s_monitor.adc.id;
    s_diagnostics.adc_last_error = s_monitor.adc.last_error;
    s_diagnostics.adc_error_read = s_monitor.adc.current_error;
    s_diagnostics.adc_init_stage = s_monitor.adc.init_stage;
    s_diagnostics.adc_init_result = s_monitor.adc.init_result;
    s_diagnostics.adc_init_channel = s_monitor.adc.init_channel;
    s_diagnostics.adc_status = s_monitor.adc.last_status;
    s_diagnostics.adc_cal_snapshot = s_monitor.adc.cal_snapshot;
    s_diagnostics.pac_last_error = (uint8_t)s_monitor.pac.last_error;
    s_has_snapshot = 1U;
    taskEXIT_CRITICAL();
}

int monitor_get_diagnostics(monitor_diagnostics_t *out)
{
    if (out == NULL) return 0;
    taskENTER_CRITICAL();
    *out = s_diagnostics;
    taskEXIT_CRITICAL();
    return 1;
}

int monitor_task_prepare(void)
{
    if (s_memory_io_mutex == NULL) {
        s_memory_io_mutex = xSemaphoreCreateMutexStatic(
            &s_memory_io_mutex_storage);
    }
    s_boot_dac.bus = &i2c2_if;
    s_boot_dac.address = MCP4725_ADDR_A0_LOW;
    s_boot_dac.vdd_mv = 3300U;
    return s_memory_io_mutex != NULL ? 1 : 0;
}

static int memory_io_take(TickType_t timeout)
{
    return s_memory_io_mutex != NULL &&
           xSemaphoreTake(s_memory_io_mutex, timeout) == pdTRUE;
}

int monitor_idd_start(uint32_t duration_ms, uint32_t minimum_samples)
{
    if (duration_ms == 0U || duration_ms > INT32_MAX || minimum_samples == 0U ||
        minimum_samples > duration_ms) return IF_ERR;
    if (!memory_io_take(pdMS_TO_TICKS(MEMORY_IO_LOCK_TIMEOUT_MS))) return IF_TIMEOUT;
    int result = IF_ERR;
    if (pwr_ctrl_get_state() == PWR_CTRL_STATE_ON) {
        taskENTER_CRITICAL();
        idd_stats_poll(&s_idd_stats, HAL_GetTick());
        result = idd_stats_begin(&s_idd_stats, HAL_GetTick(), duration_ms, minimum_samples);
        taskEXIT_CRITICAL();
    }
    xSemaphoreGive(s_memory_io_mutex);
    return result;
}

void monitor_idd_cancel(void)
{
    taskENTER_CRITICAL();
    idd_stats_cancel(&s_idd_stats);
    taskEXIT_CRITICAL();
}

void monitor_idd_finish(void)
{
    taskENTER_CRITICAL();
    idd_stats_finish(&s_idd_stats);
    taskEXIT_CRITICAL();
}

int monitor_idd_get(idd_stats_t *out)
{
    if (!out) return 0;
    taskENTER_CRITICAL();
    idd_stats_poll(&s_idd_stats, HAL_GetTick());
    *out = s_idd_stats;
    taskEXIT_CRITICAL();
    return 1;
}

/*
 * 请求的now_ms由调用方在取锁前采集。锁可能等待500ms，必须补偿这段时间，
 * 否则第一轨刚打开就会被process误判成“已等够轨间延时”。保留调用方时基，
 * 只用HAL毫秒差值补偿等待，uint32_t加减也覆盖tick回绕。
 */
static int memory_io_take_for_power(uint32_t *now_ms)
{
    uint32_t lock_start_ms = HAL_GetTick();
    if (!memory_io_take(pdMS_TO_TICKS(MEMORY_IO_LOCK_TIMEOUT_MS))) {
        return 0;
    }
    *now_ms += (uint32_t)(HAL_GetTick() - lock_start_ms);
    return 1;
}

pwr_ctrl_result_t monitor_request_power_on(uint32_t now_ms)
{
    pwr_ctrl_result_t result;
    if (!memory_io_take_for_power(&now_ms))
        return PWR_CTRL_ERR_RESOURCE_BUSY;
    result = pwr_ctrl_request_on(now_ms);
    xSemaphoreGive(s_memory_io_mutex);
    return result;
}

pwr_ctrl_result_t monitor_request_training_power_on(
    ddr_memory_type_t type, const uint16_t mv[DDR_RAIL_COUNT], uint32_t now_ms)
{
    pwr_ctrl_result_t result;
    if (!memory_io_take_for_power(&now_ms)) {
        return PWR_CTRL_ERR_RESOURCE_BUSY;
    }
    /* 同一事务内选择目标并启动，防止另一任务在两步之间修改类型或开始采样。 */
    result = pwr_ctrl_request_training_on(type, mv, now_ms);
    xSemaphoreGive(s_memory_io_mutex);
    return result;
}

pwr_ctrl_result_t monitor_request_power_off(uint32_t now_ms)
{
    pwr_ctrl_result_t result;
    if (!memory_io_take_for_power(&now_ms))
        return PWR_CTRL_ERR_RESOURCE_BUSY;
    result = pwr_ctrl_request_off(now_ms);
    if (result == PWR_CTRL_OK) monitor_idd_cancel();
    xSemaphoreGive(s_memory_io_mutex);
    return result;
}

void monitor_power_process(uint32_t now_ms)
{
    /* 即使器件IO正被占用也推进截止时间，不让无新样本的窗口一直运行。 */
    taskENTER_CRITICAL();
    if (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) idd_stats_cancel(&s_idd_stats);
    idd_stats_poll(&s_idd_stats, now_ms);
    taskEXIT_CRITICAL();
    if (!memory_io_take(0U)) return;
    pwr_ctrl_process(now_ms);
    xSemaphoreGive(s_memory_io_mutex);
}

monitor_boot_result_t monitor_set_boot_mode(boot_mode_t mode)
{
    if (mode >= BOOT_MODE_COUNT) return MONITOR_BOOT_ERR_PARAM;
    if (!memory_io_take(pdMS_TO_TICKS(MEMORY_IO_LOCK_TIMEOUT_MS)))
        return MONITOR_BOOT_ERR_BUSY;
    if (pwr_ctrl_get_state() != PWR_CTRL_STATE_OFF) {
        xSemaphoreGive(s_memory_io_mutex);
        return MONITOR_BOOT_ERR_STATE;
    }
    /*
     * 从真正开始写DAC起，旧成功配置不再可信：总线报错也可能已经改变输出。
     * 参数/忙/状态拒绝没有访问器件，仍保留旧缓存；仅完整写成功才恢复有效。
     */
    s_boot_mode_valid = 0U;
    if (mcp4725_set_boot_mode(&s_boot_dac, mode) != IF_OK) {
        xSemaphoreGive(s_memory_io_mutex);
        return MONITOR_BOOT_ERR_IO;
    }
    s_boot_mode = mode;
    s_boot_mode_valid = 1U;
    xSemaphoreGive(s_memory_io_mutex);
    return MONITOR_BOOT_OK;
}

monitor_boot_result_t monitor_get_boot_mode(boot_mode_t *mode,
                                            uint16_t *target_mv)
{
    monitor_boot_result_t result = MONITOR_BOOT_OK;
    if (!mode || !target_mv) return MONITOR_BOOT_ERR_PARAM;
    if (!memory_io_take(pdMS_TO_TICKS(MEMORY_IO_LOCK_TIMEOUT_MS)))
        return MONITOR_BOOT_ERR_BUSY;
    if (!s_boot_mode_valid) {
        result = MONITOR_BOOT_ERR_NOT_SET;
    } else {
        *mode = s_boot_mode;
        if (mcp4725_boot_mode_target_mv(s_boot_mode, target_mv) != IF_OK)
            result = MONITOR_BOOT_ERR_IO;
    }
    xSemaphoreGive(s_memory_io_mutex);
    return result;
}

pwr_ctrl_result_t monitor_apply_ddr_config(
    ddr_memory_type_t type, const int16_t steps[DDR_RAIL_COUNT])
{
    uint16_t targets[DDR_RAIL_COUNT];
    pwr_ctrl_result_t result;
    pwr_ctrl_state_t state;

    /* 纯计算校验必须先于任何I2C访问，非法网络参数不能触发故障关断。 */
    if (!steps ||
        ddr_power_build_step_targets(type, steps, targets) != IF_OK) {
        return PWR_CTRL_ERR_PARAM;
    }
    if (s_memory_io_mutex == NULL)
        return PWR_CTRL_ERR_NOT_CONFIGURED;
    if (!memory_io_take(pdMS_TO_TICKS(MEMORY_IO_LOCK_TIMEOUT_MS))) {
        return PWR_CTRL_ERR_RESOURCE_BUSY;
    }

    /* 取得锁后重新检查状态，避免等待锁期间发生上下电迁移。 */
    state = pwr_ctrl_get_state();
    if (state == PWR_CTRL_STATE_OFF) {
        /* 只在RAM保存下次上电的训练目标；TPS尚未供电，不能访问I2C。 */
        result = ddr_power_configure_training(type, targets) == IF_OK ?
                 PWR_CTRL_OK : PWR_CTRL_ERR_PARAM;
    } else if (state == PWR_CTRL_STATE_ON) {
        if (ddr_power_get_memory_type() != type) {
            result = PWR_CTRL_ERR_BUSY;
        } else {
            /* PC须在RK完成DDR初始化后下发；ready强制门待双方业务握手冻结。 */
            monitor_idd_cancel(); /* 实际调压前终止旧条件窗口，写失败也可能改变电压。 */
            result = pwr_ctrl_apply_ddr_steps(steps);
        }
    } else if (state == PWR_CTRL_STATE_FAULT) {
        result = PWR_CTRL_ERR_FAULT;
    } else {
        result = PWR_CTRL_ERR_BUSY;
    }

    xSemaphoreGive(s_memory_io_mutex);
    return result;
}

int monitor_get_latest(monitor_snapshot_t *snapshot)
{
    uint8_t available;
    if (!snapshot) return 0;
    taskENTER_CRITICAL();
    available = s_has_snapshot;
    if (available) *snapshot = s_latest;
    taskEXIT_CRITICAL();
    if (available &&
        (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON ||
         (uint32_t)(HAL_GetTick() - snapshot->timestamp_ms) > MONITOR_SAMPLE_STALE_MS)) {
        /* 任务阻塞或刚下电时，不把最后一次成功值冒充当前有效测量。 */
        snapshot->memory.valid_mask = 0U;
    }
    return available;
}

/* 只有实际尝试的组才进入统计，AD未就绪的轮询不重复计入旧电压。 */
static void feed_sample_group(const memory_monitor_sample_t *sample,
                              uint8_t group, uint32_t begin_ms)
{
    uint32_t end_ms = HAL_GetTick();
    taskENTER_CRITICAL();
    (void)idd_stats_feed_groups(&s_idd_stats, begin_ms, end_ms, sample, group);
    taskEXIT_CRITICAL();
    monitor_publish_sample(sample, group, begin_ms, end_ms);
}

/* 0602仍使用单一sampleAgeMs，取两组有效测量中较老的采集起点，避免假刷新。 */
static void update_snapshot_age(monitor_snapshot_t *snapshot, uint32_t voltage_ms,
                                uint32_t current_ms, uint32_t now_ms)
{
    uint32_t age_ms = 0U;
    const uint8_t groups[] = {MEMORY_MONITOR_VALID_VOLTAGE, MEMORY_MONITOR_VALID_IDD};
    const uint32_t times[] = {voltage_ms, current_ms};
    for (unsigned i = 0U; i < 2U; ++i) {
        if ((snapshot->memory.valid_mask & groups[i]) == 0U) continue;
        uint32_t group_age_ms = (uint32_t)(now_ms - times[i]);
        if (group_age_ms > MONITOR_SAMPLE_STALE_MS) {
            snapshot->memory.valid_mask &= (uint8_t)~groups[i];
        } else if (group_age_ms > age_ms) {
            age_ms = group_age_ms;
        }
    }
    snapshot->timestamp_ms = now_ms - age_ms;
}

void StartMonitorTask(void *argument)
{
    monitor_snapshot_t snapshot = {0};
    uint32_t last_init_ms = 0U, last_adc_ms = 0U, last_status_ms = 0U;
    uint32_t voltage_ms = 0U, current_ms = 0U;
    uint8_t was_on = 0U;
    (void)argument;

    /* B1：PAC使用PH4/PH5软件I2C；AD使用SPI4/PE11，不占B2保活SPI2。 */
    memory_monitor_configure(&s_monitor, &i2c4_if, &spi4_if,
                             B1_SPI4_NCS_PORT, B1_SPI4_NCS_PIN, monitor_wait_ms);
    vTaskDelay(pdMS_TO_TICKS(1000U));

    for (;;) {
        TickType_t delay = pdMS_TO_TICKS(MONITOR_IDLE_DELAY_MS);
        if (memory_io_take(pdMS_TO_TICKS(MEMORY_IO_LOCK_TIMEOUT_MS))) {
            snapshot.power_state = pwr_ctrl_get_state();
            if (snapshot.power_state == PWR_CTRL_STATE_ON) {
                uint32_t now_ms = HAL_GetTick();
                if (!was_on || (uint32_t)(now_ms - last_init_ms) >= MONITOR_INIT_RETRY_MS) {
                    /* 初始化失败限频重试；就绪器件不会被另一器件的故障重置。 */
                    (void)memory_monitor_init(&s_monitor);
                    last_init_ms = HAL_GetTick();
                }
                if (!was_on) {
                    last_adc_ms = HAL_GetTick() - MONITOR_ADC_POLL_MS;
                    last_status_ms = HAL_GetTick() - MONITOR_STATUS_PERIOD_MS;
                    was_on = 1U;
                }

                /* PAC保留完整转换精度、REFRESH_V稳定等待及配置回读检查。 */
                uint8_t ready_groups = memory_monitor_ready_groups(&s_monitor);
                if ((ready_groups & MEMORY_MONITOR_VALID_IDD) != 0U) {
                    uint32_t begin_ms = HAL_GetTick();
                    (void)memory_monitor_read_current(&s_monitor, &snapshot.memory);
                    if ((snapshot.memory.valid_mask & MEMORY_MONITOR_VALID_IDD) != 0U)
                        current_ms = begin_ms;
                    feed_sample_group(&snapshot.memory, MEMORY_MONITOR_VALID_IDD, begin_ms);
                } else {
                    snapshot.memory.valid_mask &= (uint8_t)~MEMORY_MONITOR_VALID_IDD;
                }

                now_ms = HAL_GetTick();
                if ((ready_groups & MEMORY_MONITOR_VALID_VOLTAGE) != 0U &&
                    (uint32_t)(now_ms - last_adc_ms) >= MONITOR_ADC_POLL_MS) {
                    uint32_t begin_ms = now_ms;
                    int result = memory_monitor_poll_voltage(&s_monitor, &snapshot.memory,
                                                             now_ms, &begin_ms);
                    last_adc_ms = HAL_GetTick();
                    if (result != 0) {
                        if (result > 0) voltage_ms = begin_ms;
                        feed_sample_group(&snapshot.memory, MEMORY_MONITOR_VALID_VOLTAGE, begin_ms);
                    }
                } else if ((ready_groups & MEMORY_MONITOR_VALID_VOLTAGE) == 0U) {
                    snapshot.memory.valid_mask &= (uint8_t)~MEMORY_MONITOR_VALID_VOLTAGE;
                }

                now_ms = HAL_GetTick();
                if ((uint32_t)(now_ms - last_status_ms) >= MONITOR_STATUS_PERIOD_MS) {
                    /* TPS读清状态独立低频读取，不在每个电流样本后重复访问三颗器件。 */
                    (void)memory_monitor_read_status(&s_monitor, &snapshot.memory);
                    last_status_ms = HAL_GetTick();
                }
                delay = 1U; /* 每轮让出一个RTOS节拍；真实间隔还包含总线与调度耗时。 */
            } else {
                memory_monitor_invalidate(&s_monitor);
                monitor_idd_cancel();
                memset(&snapshot.memory, 0, sizeof(snapshot.memory));
                was_on = 0U;
            }
            xSemaphoreGive(s_memory_io_mutex);
        } else {
            snapshot.power_state = pwr_ctrl_get_state();
            snapshot.memory.valid_mask = 0U;
        }

        /* 所有本轮读取交付后再推进IDD窗口。 */
        monitor_publish_sample(NULL, 0U, HAL_GetTick(), HAL_GetTick());
        update_snapshot_age(&snapshot, voltage_ms, current_ms, HAL_GetTick());
        publish_snapshot(&snapshot);
        task_watchdog_progress(TASK_WATCHDOG_MONITOR);
        vTaskDelay(delay);
    }
}
