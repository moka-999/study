/* B1三路DDR电压、电流/功率和TPS状态的统一快照。 */
#ifndef MEMORY_MONITOR_H
#define MEMORY_MONITOR_H

#include "ad7124.h"
#include "pac1934.h"
#include <stdint.h>

#define MEMORY_MONITOR_RAIL_COUNT       3U
#define MEMORY_MONITOR_VALID_VOLTAGE    (1U << 0)
#define MEMORY_MONITOR_VALID_IDD        (1U << 1)
#define MEMORY_MONITOR_VALID_TPS_STATUS (1U << 2)
#define MEMORY_MONITOR_VALID_ALL        (MEMORY_MONITOR_VALID_VOLTAGE | \
                                         MEMORY_MONITOR_VALID_IDD | \
                                         MEMORY_MONITOR_VALID_TPS_STATUS)

typedef struct {
    pac1934_t pac;
    ad7124_t adc;
    uint8_t pac_ready;
    uint8_t adc_ready;
} memory_monitor_t;

typedef struct {
    uint8_t valid_mask; /* 某器件失败时，仍保留其他有效数据。 */
    int32_t voltage_mv[MEMORY_MONITOR_RAIL_COUNT];
    pac1934_measurement_t idd[MEMORY_MONITOR_RAIL_COUNT];
    uint8_t tps_status[MEMORY_MONITOR_RAIL_COUNT];
} memory_monitor_sample_t;

/* 只绑定板级总线与引脚，不发起任何物理访问。 */
void memory_monitor_configure(memory_monitor_t *monitor,
                              i2c_if_t *pac_bus, spi_if_t *adc_bus,
                              GPIO_TypeDef *adc_cs_port, uint16_t adc_cs_pin,
                              void (*wait_ms)(uint32_t ms));

/*
 * 电源进入ON后调用；可重复调用，仅重试未就绪器件，不重置已就绪器件。
 * PAC和AD任一失败返回IF_ERR；就绪标志由本模块管理，任务不直接修改。
 * 为同步接口，可能执行I2C/SPI和wait_ms；必须在任务上下文串行使用。
 */
int memory_monitor_init(memory_monitor_t *monitor);

/*
 * 清空并生成一次快照；返回IF_ERR时仍可按valid_mask使用部分数据。
 * valid_mask按器件类别生效；未置位数组不代表有效的零电压/零电流。
 * 三路顺序固定VDD1/VDD2/VDDQ，voltage_mv单位mV，idd单位见PAC类型。
 */
int memory_monitor_sample(memory_monitor_t *monitor,
                          memory_monitor_sample_t *sample);

/* 下电后清除就绪态，下一次上电必须重新读ID和配置。 */
void memory_monitor_invalidate(memory_monitor_t *monitor);

#endif
