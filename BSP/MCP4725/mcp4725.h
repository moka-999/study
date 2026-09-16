/*
 * MCP4725 12位DAC与RK3588 Boot模式映射
 *
 * B1原理图：I2C2 PF0/PF1，A0接地，7位地址0x60；
 * VOUT连接BOOT_SARADC_IN0。初版只使用Fast Write更新DAC寄存器，
 * 不写EEPROM，避免模式切换消耗EEPROM寿命。
 */
#ifndef MCP4725_H
#define MCP4725_H

#include "hal_if.h"
#include <stdint.h>

#define MCP4725_ADDR_A0_LOW  0x60U
#define MCP4725_DAC_MAX_CODE 0x0FFFU

typedef enum {
    MCP4725_POWER_NORMAL = 0,
    MCP4725_POWER_DOWN_1K,
    MCP4725_POWER_DOWN_100K,
    MCP4725_POWER_DOWN_500K,
} mcp4725_power_mode_t;

typedef enum {
    BOOT_MODE_USB_MASKROM = 0,
    BOOT_MODE_SD_CARD,
    BOOT_MODE_EMMC,
    BOOT_MODE_FSPI_M0,
    BOOT_MODE_FSPI_M1,
    BOOT_MODE_FSPI_M2,
    BOOT_MODE_EMMC_SD,
    BOOT_MODE_COUNT,
} boot_mode_t;

typedef struct {
    i2c_if_t *bus;
    uint8_t address;
    uint16_t vdd_mv; /* 真板可改为实测3V3，用于DAC码标定。 */
} mcp4725_t;

/*
 * 写接口为同步I2C操作，任务上下文调用，地址为未左移7位地址。
 * 调用方负责设备/总线串行化；本驱动不控制RK复位或上电时序。
 * IF_OK只表示DAC写入成功，不能当成RK已经按指定介质启动的确认。
 */
/**
 * @brief 通过Fast Write更新DAC寄存器和掉电位，不写EEPROM。
 * @param[in] device 非NULL句柄，含有效write_bytes回调及未左移7位地址。
 * @param[in] code 12位DAC码，0..4095。
 * @param[in] power_mode 有效mcp4725_power_mode_t。
 * @return 参数错误为IF_ERR；否则返回底层同步I2C结果，IF_OK表示写入成功。
 * @note 此原始码接口不限制Boot 1.8V，也不检查RK电源状态；Boot业务应走
 *       monitor_set_boot_mode，调用方负责串行访问。
 */
int mcp4725_write_fast(mcp4725_t *device, uint16_t code,
                       mcp4725_power_mode_t power_mode);

/**
 * @brief 按当前公式将目标mV四舍五入映射为0..4095 DAC码。
 * @param[in] output_mv 目标mV，不大于vdd_mv。
 * @param[in] vdd_mv 非0供电mV，可用实测值标定。
 * @param[out] code 非NULL输出，失败保持原值。
 * @return IF_OK成功；IF_ERR空指针、零供电或目标超供电。
 * @note 使用output_mv * 4095 / vdd_mv的当前实现；不访问硬件，不限制Boot电压。
 */
int mcp4725_mv_to_code(uint16_t output_mv, uint16_t vdd_mv, uint16_t *code);

/**
 * @brief 查询Boot模式表的目标电压。
 * @param[in] mode 有效boot_mode_t。
 * @param[out] target_mv 非NULL目标mV，范围0..1800，失败保持原值。
 * @return IF_OK成功；IF_ERR空指针或模式超出支持范围。
 */
int mcp4725_boot_mode_target_mv(boot_mode_t mode, uint16_t *target_mv);

/**
 * @brief 将Boot模式换算为DAC码并以正常功耗模式同步写入。
 * @param[in] device 非NULL句柄，vdd_mv须有效，地址为未左移7位地址。
 * @param[in] mode 有效boot_mode_t，表内目标不超过1800mV。
 * @return IF_OK写入成功；IF_ERR参数/换算失败，或透传底层I2C错误。
 * @note 不控制电源/复位，不验证实际VOUT；上层必须保证OFF并串行化访问。
 */
int mcp4725_set_boot_mode(mcp4725_t *device, boot_mode_t mode);

#endif
