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
    uint8_t voltage_collecting; /* 已开始一轮扫描，包括等待第一条转换的阶段。 */
    uint8_t voltage_mask;       /* 本轮已经取得的CH0..2，只有全部有效才交付。 */
    uint32_t voltage_begin_ms;  /* 本轮首次推进时刻；用于统计窗口和扫描超时。 */
    int32_t voltage_pending_mv[MEMORY_MONITOR_RAIL_COUNT];
} memory_monitor_t;

typedef struct {
    uint8_t valid_mask; /* 某器件失败时，仍保留其他有效数据。 */
    int32_t voltage_mv[MEMORY_MONITOR_RAIL_COUNT]; /* VDD1/VDD2/VDDQ 的 AD7124 输入端电压，单位 mV。 */
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

/** 返回已初始化的VOLTAGE/IDD组位；NULL返回0，只读，不访问总线。 */
uint8_t memory_monitor_ready_groups(const memory_monitor_t *monitor);

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
 * @brief 独立读取PAC的三轨新快照，只更新sample中的IDD组。
 * @return IF_OK本次刷新、稳定等待及配置校验均成功；IF_ERR失败。
 * @note 保留原始码及current_ua精度；失败清IDD有效位，不影响电压/TPS组。
 *       同步串行调用，刷新后仍等待数据稳定；不能宣称读到了全部内部转换。
 */
int memory_monitor_read_current(memory_monitor_t *monitor,
                                memory_monitor_sample_t *sample);

/**
 * @brief 推进高精度AD三轨扫描，每次最多读取一个已就绪通道。
 * @param[in,out] monitor 已配置的句柄，内部跨调用保存未收齐的通道。
 * @param[in,out] sample 完整成功才替换电压组；等待时保留上次完整电压。
 * @param[in] now_ms 本次调用起点的毫秒时刻，允许uint32_t回绕。
 * @param[out] begin_ms 非NULL；返回非0时给出本轮扫描起点，供窗口过滤。
 * @return 1取得新完整三轨；0等待或尚未收齐；IF_ERR器件/参数异常；
 *         IF_TIMEOUT一轮超过1000ms，须重新初始化ADC。
 * @note FS=384、Sinc4不变。失败清电压有效位及未完成扫描，不影响PAC；
 *       返回0不是失败样本，也不能把缓存电压再次累计到统计中。
 */
int memory_monitor_poll_voltage(memory_monitor_t *monitor,
                                memory_monitor_sample_t *sample,
                                uint32_t now_ms, uint32_t *begin_ms);

/**
 * @brief 独立读取三颗TPS状态，只更新对应组；读取会清器件状态。
 * @return IF_OK全部成功；IF_ERR任一失败并清TPS有效位，其余两组保持。
 */
int memory_monitor_read_status(memory_monitor_t *monitor,
                               memory_monitor_sample_t *sample);

/**
 * @brief 下电后清除器件就绪标志，使下次上电重新初始化。
 * @param[in,out] monitor 句柄，NULL时不操作。
 * @note 不访问硬件、不清除总线绑定，也不主动使设备掉电。
 */
void memory_monitor_invalidate(memory_monitor_t *monitor);

#endif
