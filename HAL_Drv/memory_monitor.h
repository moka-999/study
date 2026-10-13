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

/**
 * @brief 清零监测句柄并绑定PAC/ADC资源，不访问物理器件。
 * @param[out] monitor 输出句柄，NULL时不操作。
 * @param[in] pac_bus 借用PAC I2C接口，须在使用期间存活。
 * @param[in] adc_bus 借用AD7124 SPI接口，须在使用期间存活。
 * @param[in] adc_cs_port ADC片选GPIO端口。
 * @param[in] adc_cs_pin ADC片选GPIO位掩码。
 * @param[in] wait_ms 毫秒等待回调；PAC批量读取要求非NULL。
 * @note 本函数不检查所绑定资源是否可用；初始化和采样阶段才报告错误。
 */
void memory_monitor_configure(memory_monitor_t *monitor,
                              i2c_if_t *pac_bus, spi_if_t *adc_bus,
                              GPIO_TypeDef *adc_cs_port, uint16_t adc_cs_pin,
                              void (*wait_ms)(uint32_t ms));

/**
 * @brief 在电源ON后初始化尚未就绪的PAC和AD7124。
 * @param[in,out] monitor 非NULL、已绑定有效总线与片选的句柄。
 * @return IF_OK两颗器件都已就绪；IF_ERR参数或任一器件初始化失败。
 * @note 同步I2C/SPI及等待，任务上下文串行调用。失败器件可独立重试，
 *       不重置另一已就绪器件；就绪标志由本模块管理。
 */
int memory_monitor_init(memory_monitor_t *monitor);

/**
 * @brief 采集三轨电压、IDD和TPS状态，按类别标记有效性。
 * @param[in,out] monitor 非NULL句柄，须先初始化；设备访问会刷新/读取状态。
 * @param[out] sample 非NULL快照，两个参数有效后先清零。
 * @return IF_OK三类均有效；IF_ERR参数无效或至少一类失败，仍可按valid_mask
 *         使用其他有效类别；未置位数组不能解释为实测零值。
 * @note 顺序固定VDD1/VDD2/VDDQ；voltage_mv为mV，IDD为mV/mA/mW。
 *       同步串行调用；读取TPS会清状态。调用方须协调采样和掉电。
 */
int memory_monitor_sample(memory_monitor_t *monitor,
                          memory_monitor_sample_t *sample);

/**
 * @brief 下电后清除器件就绪标志，使下次上电重新初始化。
 * @param[in,out] monitor 句柄，NULL时不操作。
 * @note 不访问硬件、不清除总线绑定，也不主动使设备掉电。
 */
void memory_monitor_invalidate(memory_monitor_t *monitor);

#endif
