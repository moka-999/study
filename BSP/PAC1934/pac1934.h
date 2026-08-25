/*
 * ===================================================================
 * PAC1934T — 4 通道电流/功率监视器 (Microchip DS20005850)
 * ===================================================================
 *
 * 硬件（B1/MB360 原理图 P22）：
 *   I2C4 (PH4/5), 地址 0x2C (ADDRSEL=5.23K→GND)
 *   SENSE1 = VCC_1V8_S3_LPDDR4X  (0.01Ω)  → VDD1 电流
 *   SENSE2 = VDD2_DDR_S3        (0.01Ω)  → VDD2 电流
 *   SENSE3 = VDDQ_DDR           (0.01Ω)  → VDDQ 电流
 *   SENSE4 未用 → 禁用
 *
 * 换算 (手册 Equation 4-1/4-4/4-7, unipolar):
 *   Vbus  = VBUS_reg  × 32V / 2^16          (LSB = 488.3µV)
 *   FSC   = 100mV / Rsense
 *   I     = FSC × VSENSE_reg / 2^16         (LSB = FSC/65536)
 *   P     = PowerFSR × VPOWER_reg / 2^28
 *   PowerFSR = (100mV/Rsense) × 32V
 *
 * 初始化: CHANNEL_DIS 禁用 SENSE4 → CTRL 采样率 → REFRESH 启动
 * 读取:    REFRESH 后 1ms 读 VBUS/VSENSE/VPOWER 结果寄存器
 *
 * V2 验证: i2c4_if (PF14/15 引出) 挂 PAC 模块
 * B1:       i2c4_if 使用PH4/PH5软件开漏GPIO后直连U204
 * ===================================================================
 */
#ifndef __PAC1934_H
#define __PAC1934_H
#include "hal_if.h"
#include <stdint.h>

/* 寄存器地址 */
#define PAC1934_REG_REFRESH       0x00   /* 命令: 刷新数据+复位累加器 (Send Byte) */
#define PAC1934_REG_CTRL          0x01   /* 采样率[7:6]/SLEEP[5]/SING[4]/ALERT */
#define PAC1934_REG_ACC_COUNT     0x02   /* 累加计数 (3B) */
#define PAC1934_REG_VPOWER_ACC1   0x03   /* 功率累加器 ch1 (6B) ... 06h=ch4 */
#define PAC1934_REG_VBUS1         0x07   /* 总线电压 ch1 (2B) ... 0Ah=ch4 */
#define PAC1934_REG_VSENSE1       0x0B   /* 采样电压 ch1 (2B) ... 0Eh=ch4 */
#define PAC1934_REG_VBUS1_AVG     0x0F   /* 平均总线电压 ch1 (2B) ... 12h=ch4 */
#define PAC1934_REG_VSENSE1_AVG   0x13   /* 平均采样电压 ch1 (2B) ... 16h=ch4 */
#define PAC1934_REG_VPOWER1       0x17   /* 功率结果 ch1 (4B) ... 1Ah=ch4 */
#define PAC1934_REG_CHANNEL_DIS   0x1C   /* 禁用通道: bit7=ch1..bit4=ch4 */
#define PAC1934_REG_NEG_PWR       0x1D   /* 双向电流/双极性电压配置 */
#define PAC1934_REG_REFRESH_G     0x1E   /* 刷新+读累加器并复位 */
#define PAC1934_REG_REFRESH_V     0x1F   /* 刷新不复位累加器 */
#define PAC1934_REG_CTRL_ACT      0x21   /* CTRL 实际状态 */
#define PAC1934_REG_CTRL_LAT      0x24   /* CTRL 锁存 (报警清除) */
#define PAC1934_REG_PRODUCT_ID    0xFD   /* 产品 ID */
#define PAC1934_REG_MANUF_ID      0xFE   /* 厂商 ID = 0x5D */
#define PAC1934_REG_REV_ID        0xFF   /* 版本 ID */

/* CTRL 位 (01h) */
#define PAC1934_CTRL_SAMPLE_RATE(x)  (((x) & 0x3) << 6)  /* 00=1024 01=256 10=64 11=8 SPS */
#define PAC1934_CTRL_SLEEP           (1u << 5)
#define PAC1934_CTRL_SING            (1u << 4)
#define PAC1934_CTRL_ALERT_PIN       (1u << 3)
#define PAC1934_CTRL_ALERT_CC        (1u << 2)
#define PAC1934_CTRL_OVF_ALERT       (1u << 1)

/* 产品/厂商 ID 值 */
#define PAC1934_PRODUCT_ID          0x5B   /* PAC1934 */
#define PAC1934_MANUF_ID            0x5D   /* Microchip */
#define PAC1934_CH_MASK_ALL          0x0F   /* bit0-3 = ch1-4 使能位 */

/* CHANNEL_DIS(0x1C) 的通道关闭位实际位于bit7..4，不是bit3..0。 */
#define PAC1934_CH_DISABLE_BITS(mask)  ((uint8_t)((~(mask) & PAC1934_CH_MASK_ALL) << 4))

typedef void (*pac1934_wait_ms_fn)(uint32_t ms);

typedef struct {
    i2c_if_t *bus;       /* I2C 总线接口 (i2c4_if) */
    uint8_t   addr;      /* I2C 地址 (0x2C, ADDRSEL 决定) */
    uint32_t  rsense_uohm; /* 采样电阻 (µΩ, 默认 0.01Ω=10000) */
    uint8_t   active_mask; /* 初始化成功后保存实际启用的通道，bit0=ch1 */
    pac1934_wait_ms_fn wait_ms; /* REFRESH_V后必须等待至少1ms */
} pac1934_t;

/* 单通道测量结果 */
typedef struct {
    uint16_t vbus;    /* 总线电压寄存器原始值 */
    uint16_t vsense;  /* 采样电压寄存器原始值 */
    uint32_t vpower;  /* 功率寄存器原始值 */
} pac1934_chan_t;

/* MCU侧IDD采样结果；IDD激励状态本身由RK按照JEDEC测试条件产生。 */
typedef struct {
    uint32_t vbus_mv;
    uint32_t current_ma;
    uint32_t power_mw;
} pac1934_measurement_t;

/* 初始化: 读 ID 校验 + 禁用未用通道 + 采样率 + 启动转换 */
int  pac1934_init(pac1934_t *d, uint8_t ch_mask, uint8_t sample_rate);

/* 刷新并读取启用通道；内部通过wait_ms保证REFRESH_V后等待至少1ms。 */
int  pac1934_read_all(pac1934_t *d, pac1934_chan_t *ch);

/* 读单通道 (ch=0..3) */
int  pac1934_read_ch(pac1934_t *d, uint8_t ch, pac1934_chan_t *out);

/* 一次刷新后读取并换算全部启用通道，禁用通道结果保持为0。 */
int  pac1934_measure_all(pac1934_t *d, pac1934_measurement_t *out);

/* 换算: 电压 mV / 电流 mA / 功率 mW */
uint32_t pac1934_vbus_mv(uint16_t vbus_reg);
uint32_t pac1934_current_ma(uint16_t vsense_reg, uint32_t rsense_uohm);
uint32_t pac1934_power_mw(uint32_t vpower_reg, uint32_t rsense_uohm);

#endif
