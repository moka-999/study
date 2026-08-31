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
/* Fast Write直接更新DAC寄存器和掉电位，不修改EEPROM。 */
int mcp4725_write_fast(mcp4725_t *device, uint16_t code,
                       mcp4725_power_mode_t power_mode);

/* 按实测VDD把毫伏值四舍五入换算为12位DAC码。 */
int mcp4725_mv_to_code(uint16_t output_mv, uint16_t vdd_mv, uint16_t *code);

/* 查询原理图Table 1的目标电压，非法模式返回IF_ERR。 */
int mcp4725_boot_mode_target_mv(boot_mode_t mode, uint16_t *target_mv);

/* 设置RK3588 Boot模式；所有合法模式都不高于1.8V。 */
int mcp4725_set_boot_mode(mcp4725_t *device, boot_mode_t mode);

#endif
