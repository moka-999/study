/*
 * DDR电源监测任务
 *
 * 仅在pwr_ctrl进入ON后初始化PAC1934和AD7124并采集三路快照。
 * B1时序未冻结前pwr_ctrl未配置，默认OFF，不访问采样器件。
 * 快照留给新PC–MCU JSON业务层读取，不再向旧TLV TCP队列发文本。
 */
#include "monitor_task.h"
#include "ota_app.h"
#include "board_interfaces.h"
#include "mcp4725.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "main.h"
#include <string.h>

static memory_monitor_t s_monitor;
static monitor_snapshot_t s_latest;
static uint8_t s_has_snapshot;
static StaticSemaphore_t s_memory_io_mutex_storage;
static SemaphoreHandle_t s_memory_io_mutex;
static mcp4725_t s_boot_dac;
static boot_mode_t s_boot_mode;
static uint8_t s_boot_mode_valid;
static uint8_t s_ota_maintenance;

#define MEMORY_IO_LOCK_TIMEOUT_MS 500U

static void monitor_wait_ms(uint32_t milliseconds)
{
    vTaskDelay(pdMS_TO_TICKS(milliseconds));
}

static void publish_snapshot(const monitor_snapshot_t *snapshot)
{
    taskENTER_CRITICAL();
    s_latest = *snapshot;
    s_has_snapshot = 1U;
    taskEXIT_CRITICAL();
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
    result = s_ota_maintenance || !ota_app_is_confirmed() ? PWR_CTRL_ERR_BUSY : pwr_ctrl_request_on(now_ms);
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
    result = s_ota_maintenance || !ota_app_is_confirmed() ? PWR_CTRL_ERR_BUSY :
             pwr_ctrl_request_training_on(type, mv, now_ms);
    xSemaphoreGive(s_memory_io_mutex);
    return result;
}

pwr_ctrl_result_t monitor_request_power_off(uint32_t now_ms)
{
    pwr_ctrl_result_t result;
    if (!memory_io_take_for_power(&now_ms))
        return PWR_CTRL_ERR_RESOURCE_BUSY;
    result = pwr_ctrl_request_off(now_ms);
    xSemaphoreGive(s_memory_io_mutex);
    return result;
}

void monitor_power_process(uint32_t now_ms)
{
    if (!memory_io_take(0U)) return;
    pwr_ctrl_process(now_ms);
    xSemaphoreGive(s_memory_io_mutex);
}

monitor_boot_result_t monitor_set_boot_mode(boot_mode_t mode)
{
    if (mode >= BOOT_MODE_COUNT) return MONITOR_BOOT_ERR_PARAM;
    if (!memory_io_take(pdMS_TO_TICKS(MEMORY_IO_LOCK_TIMEOUT_MS)))
        return MONITOR_BOOT_ERR_BUSY;
    if (s_ota_maintenance || !ota_app_is_confirmed() || pwr_ctrl_get_state() != PWR_CTRL_STATE_OFF) {
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

    /*
     * 取得锁后重新检查状态，避免等待锁期间发生上下电迁移。当前没有独立
     * DDR-ready输入；PC必须在RK已完成内存初始化后才发送非零拉偏命令，
     * 后续冻结RK握手时再把该确认纳入此处的强制门控。
     */
    if (s_ota_maintenance || !ota_app_is_confirmed()) {
        xSemaphoreGive(s_memory_io_mutex);
        return PWR_CTRL_ERR_BUSY;
    }
    state = pwr_ctrl_get_state();
    if (state == PWR_CTRL_STATE_OFF) {
        if (steps[DDR_RAIL_VDD1] != 0 || steps[DDR_RAIL_VDD2] != 0 ||
            steps[DDR_RAIL_VDDQ] != 0) {
            result = PWR_CTRL_ERR_BUSY;
        } else {
            result = pwr_ctrl_select_memory_type(type);
        }
    } else if (state == PWR_CTRL_STATE_ON) {
        if (ddr_power_get_memory_type() != type) {
            result = PWR_CTRL_ERR_BUSY;
        } else {
            /* PC须在RK完成DDR初始化后下发；ready强制门待双方业务握手冻结。 */
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
    return available;
}

void StartMonitorTask(void *argument)
{
    monitor_snapshot_t snapshot;
    (void)argument;

    /* B1：PAC使用PH4/PH5软件I2C；AD使用SPI4/PE11，不占B2保活SPI2。 */
    memory_monitor_configure(&s_monitor, &i2c4_if, &spi4_if,
                             B1_SPI4_NCS_PORT, B1_SPI4_NCS_PIN, monitor_wait_ms);
    vTaskDelay(pdMS_TO_TICKS(1000U));

    for (;;) {
        memset(&snapshot, 0, sizeof(snapshot));
        snapshot.timestamp_ms = HAL_GetTick();
        if (memory_io_take(pdMS_TO_TICKS(MEMORY_IO_LOCK_TIMEOUT_MS))) {
            snapshot.power_state = pwr_ctrl_get_state();
            if (snapshot.power_state == PWR_CTRL_STATE_ON) {
                /* 组合层自身管理就绪态，只重试未初始化器件。 */
                (void)memory_monitor_init(&s_monitor);
                /* 单器件失败时仍保留其他有效数据，由valid_mask标识。 */
                (void)memory_monitor_sample(&s_monitor, &snapshot.memory);
            } else {
                memory_monitor_invalidate(&s_monitor);
            }
            xSemaphoreGive(s_memory_io_mutex);
        } else {
            /* 配置命令占用器件时发布无有效测量的快照，不复用旧数据冒充新值。 */
            snapshot.power_state = pwr_ctrl_get_state();
        }

        publish_snapshot(&snapshot);
        ota_app_progress(OTA_TASK_MONITOR);
        vTaskDelay(pdMS_TO_TICKS(2000U));
    }
}

int monitor_ota_enter(void)
{
    if (!memory_io_take(pdMS_TO_TICKS(MEMORY_IO_LOCK_TIMEOUT_MS))) return -1;
    int result = -1;
    if (!s_ota_maintenance && pwr_ctrl_get_state() == PWR_CTRL_STATE_OFF) {
        s_ota_maintenance = 1U;
        result = 0;
    }
    xSemaphoreGive(s_memory_io_mutex);
    return result;
}

void monitor_ota_leave(void)
{
    if (!memory_io_take(pdMS_TO_TICKS(MEMORY_IO_LOCK_TIMEOUT_MS))) return;
    s_ota_maintenance = 0U;
    xSemaphoreGive(s_memory_io_mutex);
}
