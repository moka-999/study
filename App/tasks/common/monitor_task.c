/*
 * DDR电源监测任务
 *
 * 仅在pwr_ctrl进入ON后初始化PAC1934和AD7124并采集三路快照。
 * Apollo V2正式镜像未配置pwr_ctrl，因此不会访问不存在的器件。
 * 快照留给新PC–MCU JSON业务层读取，不再向旧TLV TCP队列发文本。
 */
#include "monitor_task.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include <string.h>

extern i2c_if_t i2c4_if;
extern spi_if_t spi2_if;

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

    /* V2开发映射：B1移植时由板级层换为I2C4软总线+SPI4/PE11。 */
    memory_monitor_configure(&s_monitor, &i2c4_if, &spi2_if,
                             GPIOB, GPIO_PIN_12, monitor_wait_ms);
    vTaskDelay(pdMS_TO_TICKS(1000U));

    for (;;) {
        memset(&snapshot, 0, sizeof(snapshot));
        snapshot.timestamp_ms = HAL_GetTick();
        snapshot.power_state = pwr_ctrl_get_state();

        if (snapshot.power_state == PWR_CTRL_STATE_ON) {
            if (!s_monitor.pac_ready || !s_monitor.adc_ready)
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
