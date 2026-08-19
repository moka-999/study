/*
 * ===================================================================
 * DDR 电源管理 — 三路 TPS62868 (VDD1/VDD2/VDDQ) 统一控制
 * ===================================================================
 *
 * B2 原理图 (20260811):
 *   rail 0 = VDD1  VCC_1V8_S3_LPDDR4X  1.8V  TPS62868 U4025  I2C1 (PB8/9)
 *   rail 1 = VDD2  VDD2_DDR_S3        1.1V  TPS62868 U4026  I2C2 (B2: PF0/1)
 *   rail 2 = VDDQ  VDDQ_DDR           0.6V  TPS62868 U4027  I2C4 (V2: PF14/15)
 *
 * 料号 (C 版, 地址全 0x42, 因子按 BOM 后缀):
 *   VDD1 = x2C (因子2, 启动1.2V)   code=(1800/2-400)/5=100
 *   VDD2 = x1C (因子1, 启动0.9V)   code=(1100-400)/5=140
 *   VDDQ = x0C (因子0.5, 启动0.5V) code=(600/0.5-400)/5=160
 *   选料原则: 启动电压 ≤ 目标电压 → DDR 上电单调爬升, 无过压
 *
 * 上电流程: PG11(EN) 拉高 → ddr_power_init() 清STATUS+设基准电压
 * 拉偏:     ddr_power_set(mask, mv) 随时动态调压 (ramp 20mV/µs)
 *
 * V2 阶段: i2c1_if/i2c4_if 可用 (V2 已配), i2c2_if 未配置 → rail1 返回 IF_ERR
 * B2 阶段: 补 i2c2_if 后三路全通
 * ===================================================================
 */
#ifndef __DDR_POWER_H
#define __DDR_POWER_H
#include "hal_if.h"
#include "tps62868.h"
#include <stdint.h>

#define DDR_RAIL_VDD1   0
#define DDR_RAIL_VDD2   1
#define DDR_RAIL_VDDQ   2
#define DDR_RAIL_COUNT  3

#define DDR_RAIL_MASK_VDD1  (1u << 0)
#define DDR_RAIL_MASK_VDD2  (1u << 1)
#define DDR_RAIL_MASK_VDDQ  (1u << 2)
#define DDR_RAIL_MASK_ALL   (DDR_RAIL_MASK_VDD1 | DDR_RAIL_MASK_VDD2 | DDR_RAIL_MASK_VDDQ)

/* 各轨默认目标电压 (mV) */
#define DDR_VDD1_DEFAULT_MV  1800
#define DDR_VDD2_DEFAULT_MV  1100
#define DDR_VDDQ_DEFAULT_MV  600

/* 初始化: 清 STATUS 残留标志 + 写基准电压 + ramp 最快 (EN 拉高后调用) */
int  ddr_power_init(void);

/* 动态调压: mask 选轨 (bit0=VDD1,bit1=VDD2,bit2=VDDQ), mv[3] 各轨目标 mV */
int  ddr_power_set(uint8_t rail_mask, const uint16_t *mv);

/* 读某轨 STATUS (thermal/HICCUP/UVLO) */
int  ddr_power_read_status(uint8_t rail, uint8_t *flags);

/* 查询某轨当前目标电压 */
uint16_t ddr_power_get_mv(uint8_t rail);

#endif