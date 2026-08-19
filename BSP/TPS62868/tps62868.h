/*
 * ===================================================================
 * TPS62868/TPS62869 — 4A/6A 同步降压 I2C 可调 (TI SLVSFS3B)
 * ===================================================================
 *
 * 硬件 (B2 原理图 20260811):
 *   U4025 = I2C1 → VCC_1V8_S3_LPDDR4X (VDD1, 1.8V)
 *   U4026 = I2C2 → VDD2_DDR_S3       (VDD2, 1.1V)
 *   U4027 = I2C4 → VDDQ_DDR          (VDDQ, 0.6V)
 *   EN 全部接 PG11 (10K 下拉 → 默认 OFF, 需先 pwr_ctrl 上电)
 *   VIN = VCC4V0_SYS
 *
 * 器件版本 (料号后缀):  地址固定 0x42 (Table 8-3, C 版出厂固定)
 *   因子: x0C=0.5 / x1C=1 / x2C=2   (三颗都在独立 I2C 总线, 地址相同不冲突)
 *
 * VOUT 换算 (Table 8-6/8-7):
 *   VOUT = (400 + 5*code) * factor mV,  code = (VOUT/factor - 400)/5
 *   code 范围 0x00..0xFF → 单因子满量程 400mV..1675mV
 *   (VDD1=1.8V 需 factor=2 才够; VDDQ=0.6V 需 factor≤1, 具体看 BOM 料号)
 *
 * 寄存器:
 *   0x01 VOUT1    [7:0] VO1_SET   RW (VID=低 时生效)
 *   0x02 VOUT2    [7:0] VO2_SET   RW (VID=高 时生效, 默认 0x64)
 *   0x03 CONTROL  WO (bit7=reset, bit6=FPWM-during-change, bit5=sw-enable,
 *                    bit4=FPWM, bit3=output-discharge, bit2=HICCUP, bit1:0=ramp)
 *   0x05 STATUS   R  (bit4=thermal-warning>130C, bit3=HICCUP, bit0=UVLO; 读后清)
 *
 * V2 验证: 模块挂 I2C1 (i2c1_if, 0x42)
 * B2:      三颗分别挂 i2c1_if/i2c2_if/i2c4_if (需补 I2C2 外设配置 + i2c2_if)
 * ===================================================================
 */
#ifndef __TPS62868_H
#define __TPS62868_H
#include "hal_if.h"
#include <stdint.h>

/* 寄存器地址 */
#define TPS62868_REG_VOUT1      0x01
#define TPS62868_REG_VOUT2      0x02
#define TPS62868_REG_CONTROL    0x03
#define TPS62868_REG_STATUS     0x05

/* C 版 I2C 地址 (出厂固定) */
#define TPS62868_ADDR           0x42

/* 电压因子 (x10 整数避免浮点): x0C=5, x1C=10, x2C=20 */
#define TPS62868_FACTOR_0X5     5
#define TPS62868_FACTOR_1X0     10
#define TPS62868_FACTOR_2X0     20

/* CONTROL 位 */
#define TPS62868_CTL_RESET              (1u << 7)
#define TPS62868_CTL_FPWM_DURING_CHANGE (1u << 6)
#define TPS62868_CTL_SW_ENABLE          (1u << 5)
#define TPS62868_CTL_FPWM               (1u << 4)
#define TPS62868_CTL_OUT_DISCHARGE      (1u << 3)
#define TPS62868_CTL_HICCUP             (1u << 2)
#define TPS62868_CTL_RAMP_SPEED(x)      (((x) & 0x3) << 0)
#define TPS62868_RAMP_20MV_US   0   /* 20 mV/us */
#define TPS62868_RAMP_10MV_US   1   /* 10 mV/us */
#define TPS62868_RAMP_5MV_US    2   /* 5  mV/us */
#define TPS62868_RAMP_1MV_US    3   /* 1  mV/us (默认) */

/* STATUS 位 (读后自动清零) */
#define TPS62868_STAT_THERMAL_WARN  (1u << 4)
#define TPS62868_STAT_HICCUP        (1u << 3)
#define TPS62868_STAT_UVLO          (1u << 0)

typedef struct {
    i2c_if_t *bus;       /* I2C 总线接口 (i2c1_if/i2c2_if/i2c4_if) */
    uint8_t   addr;      /* I2C 地址, C 版固定 0x42                  */
    uint8_t   factor_x10; /* 电压因子 x10 (5/10/20, 按 BOM 料号)     */
    uint8_t   ctl_cache; /* CONTROL 软件镜像 (write-only 寄存器, 默认 0x6F) */
} tps62868_t;

/* ── 3.1 地址扫描 (返回找到的设备数) ── */
int  tps62868_scan(i2c_if_t *bus, uint8_t *addrs, uint8_t max);

/* ── 寄存器读/写 ── */
int  tps62868_read_reg (tps62868_t *d, uint8_t reg, uint8_t *val);
int  tps62868_write_reg(tps62868_t *d, uint8_t reg, uint8_t val);

/* ── 3.2 VOUT 设置 (mV→code, 按 factor 换算, 检查边界) ── */
int  tps62868_set_vout1(tps62868_t *d, uint16_t mv);   /* VID=低 生效 */
int  tps62868_set_vout2(tps62868_t *d, uint16_t mv);   /* VID=高 生效 */

/* ── 软件使能/禁用 (CONTROL bit5, 寄存器值保留) ── */
int  tps62868_enable (tps62868_t *d);
int  tps62868_disable(tps62868_t *d);

/* ── 输出放电 (CONTROL bit3, 默认使能) / 斜坡速率 (bit1:0) ── */
int  tps62868_set_discharge(tps62868_t *d, uint8_t on);
int  tps62868_set_ramp     (tps62868_t *d, uint8_t ramp);

/* ── 3.3 STATUS 读 (热预警/HICCUP/UVLO, 读后自动清零) ── */
int  tps62868_read_status (tps62868_t *d, uint8_t *flags);

/* ── 复位所有寄存器 (CONTROL bit7) ── */
int  tps62868_reset_regs  (tps62868_t *d);

#endif