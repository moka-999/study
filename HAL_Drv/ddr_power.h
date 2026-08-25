/*
 * ===================================================================
 * DDR 电源管理 — 三路 TPS62868 (VDD1/VDD2/VDDQ) 统一控制
 * ===================================================================
 *
 * B1/MB360 原理图（20260811）：
 *   rail 0 = VDD1  VCC_1V8_S3_LPDDR4X  1.8V  TPS62868 U4025  I2C1 (PB8/9)
 *   rail 1 = VDD2  VDD2_DDR_S3        1.1V  TPS62868 U4026  I2C2 (B1: PF0/1)
 *   rail 2 = VDDQ  VDDQ_DDR           0.6V  TPS62868 U4027  I2C4 (V2: PF14/15)
 *
 * 料号 (A 版, VSET/PG 电阻选择地址和启动档位):
 *   VDD1 = TPS628692A, 56.2k, 0x49, 启动1.60V  code=(1800/2-400)/5=100
 *   VDD2 = TPS628681A, 86.8k(TI 86.6k档), 0x41, 启动0.90V  code=(1100-400)/5=140
 *   VDDQ = x1A, 15.4k, 0x4F, 启动0.50V  code=(600-400)/5=40
 *   选料原则: 启动电压 ≤ 目标电压 → DDR 上电单调爬升, 无过压
 *
 * 上电流程: PG11(EN) 拉高 → ddr_power_init() 清STATUS+设基准电压
 * 拉偏:     ddr_power_set(mask, mv) 随时动态调压 (ramp 20mV/µs)
 *
 * V2 阶段: i2c1_if/i2c4_if 可用 (V2 已配), i2c2_if 未配置 → rail1 返回 IF_ERR
 * B1 阶段: 接入硬件I2C2和PH4/PH5软件开漏总线后，三路全通
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

/*
 * 被测器件类型只影响VDDQ标称值：LPDDR4为1.1V，LPDDR4X为0.6V。
 * VDD1/VDD2分别保持1.8V/1.1V。
 */
typedef enum {
    DDR_MEMORY_LPDDR4 = 0,
    DDR_MEMORY_LPDDR4X,
} ddr_memory_type_t;

/* 老化测试使用的受控拉偏档位；初始化阶段始终强制回到NOMINAL。 */
typedef enum {
    DDR_BIAS_NOMINAL = 0,
    DDR_BIAS_PLUS_5_PERCENT,
    DDR_BIAS_MINUS_5_PERCENT,
    DDR_BIAS_MINUS_10_PERCENT,
} ddr_bias_profile_t;

/* 初始化: 清 STATUS 残留标志 + 写基准电压 + ramp 最快 (EN 拉高后调用) */
int  ddr_power_init(void);

/* 仅应在整机电源OFF状态选择器件类型；下一次初始化按对应标称值上电。 */
int  ddr_power_select_memory_type(ddr_memory_type_t type);

/* 生成并应用固定拉偏档位，避免上层自行计算出不可表示或越界的电压。 */
int  ddr_power_build_profile(ddr_memory_type_t type, ddr_bias_profile_t profile,
                             uint16_t mv[DDR_RAIL_COUNT]);
int  ddr_power_apply_bias(ddr_bias_profile_t profile);

/* 动态调压: mask 选轨 (bit0=VDD1,bit1=VDD2,bit2=VDDQ), mv[3] 各轨目标 mV */
int  ddr_power_set(uint8_t rail_mask, const uint16_t *mv);

/* 读某轨 STATUS (thermal/HICCUP/UVLO) */
int  ddr_power_read_status(uint8_t rail, uint8_t *flags);

/* 查询某轨当前目标电压 */
uint16_t ddr_power_get_mv(uint8_t rail);

ddr_memory_type_t ddr_power_get_memory_type(void);
ddr_bias_profile_t ddr_power_get_bias_profile(void);

#endif
