/* B1 DDR电源与采样器件的最小组合层。 */
#include "memory_monitor.h"
#include "ddr_power.h"
#include <string.h>

void memory_monitor_configure(memory_monitor_t *monitor,
                              i2c_if_t *pac_bus, spi_if_t *adc_bus,
                              GPIO_TypeDef *adc_cs_port, uint16_t adc_cs_pin,
                              void (*wait_ms)(uint32_t ms))
{
    if (!monitor) return;
    memset(monitor, 0, sizeof(*monitor));
    monitor->pac.bus = pac_bus;
    monitor->pac.addr = PAC1934_ADDR_B1;
    monitor->pac.rsense_uohm = PAC1934_RSENSE_B1_UOHM;
    monitor->pac.wait_ms = wait_ms;
    monitor->adc.spi = adc_bus;
    monitor->adc.cs_port = adc_cs_port;
    monitor->adc.cs_pin = adc_cs_pin;
    monitor->adc.wait_ms = wait_ms;
}

int memory_monitor_init(memory_monitor_t *monitor)
{
    if (!monitor) return IF_ERR;

    /* 失败器件可独立重试，不重置已就绪的另一颗器件。 */
    if (!monitor->pac_ready && monitor->pac.bus)
        monitor->pac_ready = pac1934_init(&monitor->pac,
                                          PAC1934_CH_MASK_B1_RAILS, 0U) == IF_OK;
    if (!monitor->adc_ready && monitor->adc.spi && monitor->adc.cs_port)
        monitor->adc_ready = ad7124_init(&monitor->adc) == IF_OK;
    return monitor->pac_ready && monitor->adc_ready ? IF_OK : IF_ERR;
}

int memory_monitor_sample(memory_monitor_t *monitor,
                          memory_monitor_sample_t *sample)
{
    uint8_t got = 0U;
    static const uint8_t pga[MEMORY_MONITOR_RAIL_COUNT] = {0U, 0U, 0U};
    static const uint8_t bipolar[MEMORY_MONITOR_RAIL_COUNT] = {0U, 0U, 0U};

    if (!monitor || !sample) return IF_ERR;
    memset(sample, 0, sizeof(*sample));

    if (monitor->adc_ready &&
        ad7124_read_all_mv(&monitor->adc, sample->voltage_mv,
                           MEMORY_MONITOR_RAIL_COUNT, &got, 2.5f,
                           pga, bipolar) == IF_OK &&
        got == MEMORY_MONITOR_RAIL_COUNT) {
        /*
         * B1三路采样输入都经过100R串联电阻和10k对地电阻。AD7124读到的是
         * 10k电阻上的电压，源端电压应乘(100+10000)/10000=1.01。
         * 这里在现有整数mV接口上做1.01倍四舍五入；raw转mV此前仍有小于
         * 1mV的量化误差，小于TPS的5/10mV档位。真实电阻误差和ADC增益仍须
         * 在B1首板上标定，届时再决定是否把分压系数前移到raw换算阶段。
         */
        for (uint8_t rail = 0U; rail < MEMORY_MONITOR_RAIL_COUNT; rail++) {
            int64_t scaled = (int64_t)sample->voltage_mv[rail] * 101;
            sample->voltage_mv[rail] = (int32_t)((scaled + 50) / 100);
        }
        sample->valid_mask |= MEMORY_MONITOR_VALID_VOLTAGE;
    }

    /* 故障样本不发布；下次监测独立重初始化ADC，不影响PAC和TPS。 */
    if ((sample->valid_mask & MEMORY_MONITOR_VALID_VOLTAGE) == 0U)
        monitor->adc_ready = 0U;

    /* PAC返回4通道数组，本项目只保存已启用的前3路。 */
    if (monitor->pac_ready) {
        pac1934_measurement_t all[4];
        if (pac1934_measure_all(&monitor->pac, all) == IF_OK) {
            memcpy(sample->idd, all, sizeof(sample->idd));
            sample->valid_mask |= MEMORY_MONITOR_VALID_IDD;
        } else if (monitor->pac.active_mask == 0U) {
            /* 读取失败且器件配置已失信：不发布旧值，下一监测周期重新初始化。 */
            monitor->pac_ready = 0U;
        }
    }

    for (uint8_t rail = 0U; rail < MEMORY_MONITOR_RAIL_COUNT; rail++) {
        if (ddr_power_read_status(rail, &sample->tps_status[rail]) != IF_OK)
            break;
        if (rail + 1U == MEMORY_MONITOR_RAIL_COUNT)
            sample->valid_mask |= MEMORY_MONITOR_VALID_TPS_STATUS;
    }

    return sample->valid_mask == MEMORY_MONITOR_VALID_ALL ? IF_OK : IF_ERR;
}

void memory_monitor_invalidate(memory_monitor_t *monitor)
{
    if (!monitor) return;
    monitor->pac_ready = 0U;
    monitor->adc_ready = 0U;
}
