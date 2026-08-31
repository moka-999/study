/*
 * DDR电源监测任务
 *
 * 仅在pwr_ctrl进入ON后初始化PAC1934和AD7124并采集三路快照。
 * B1时序未冻结前pwr_ctrl未配置，默认OFF，不访问采样器件。
 * 快照留给新PC–MCU JSON业务层读取，不再向旧TLV TCP队列发文本。
 */
#include "monitor_task.h"
#include "board_interfaces.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include <string.h>

static memory_monitor_t s_monitor;
static monitor_snapshot_t s_latest;
static uint8_t s_has_snapshot;

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
        snapshot.power_state = pwr_ctrl_get_state();

        if (snapshot.power_state == PWR_CTRL_STATE_ON) {
            /* 组合层自身管理就绪态，只重试未初始化器件；任务不读取内部标志。 */
            (void)memory_monitor_init(&s_monitor);
            /* 单器件失败时仍保留其他有效数据，由valid_mask标识。 */
            (void)memory_monitor_sample(&s_monitor, &snapshot.memory);
        } else {
            memory_monitor_invalidate(&s_monitor);
        }

        publish_snapshot(&snapshot);
        vTaskDelay(pdMS_TO_TICKS(2000U));
    }
}
