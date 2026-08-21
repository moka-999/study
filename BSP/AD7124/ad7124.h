/*
 * ===================================================================
 * AD7124-4 — 4ch 24-bit Σ-Δ ADC 驱动 (SPI)
 * ===================================================================
 *
 * 硬件:
 *   阿波罗 V2: SPI2 (PB13=SCK, PB14=MISO, PB15=MOSI, PB12=NCS)
 *   B1正式板: SPI4 (PE11=NSS, PE12=SCK, PE13=MISO, PE14=MOSI)
 * 移植: 换 spi_if + cs_port/cs_pin 即可
 *
 * 参考: AD7124-4 datasheet (Rev. D), ADI no-OS ad7124 driver
 * ===================================================================
 */
#ifndef __AD7124_H
#define __AD7124_H
#include "hal_if.h"
#include "main.h"
#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════
 * 寄存器地址
 * ═══════════════════════════════════════════════════════════════════ */

#define AD7124_REG_STATUS       0x00    /* R, 8-bit   */
#define AD7124_REG_ADC_CTRL     0x01    /* RW, 16-bit */
#define AD7124_REG_DATA         0x02    /* R, 24-bit  */
#define AD7124_REG_IO_CTRL1     0x03    /* RW, 24-bit */
#define AD7124_REG_IO_CTRL2     0x04    /* RW, 16-bit */
#define AD7124_REG_ID           0x05    /* R, 8-bit   */
#define AD7124_REG_ERROR        0x06    /* R, 24-bit  */
#define AD7124_REG_ERROR_EN     0x07    /* RW, 24-bit */
#define AD7124_REG_MCLK_COUNT   0x08    /* R, 8-bit   */

/* 每组 8 个: CH0-15, CFG0-7, FILTER0-7, OFFSET0-7, GAIN0-7 */
#define AD7124_REG_CH0          0x09    /* RW, 16-bit, CH0..CH15: 0x09..0x18 */
#define AD7124_REG_CFG0         0x19    /* RW, 16-bit, CFG0..CFG7: 0x19..0x20 */
#define AD7124_REG_FILTER0      0x21    /* RW, 24-bit, FILTER0..FILTER7: 0x21..0x28 */
#define AD7124_REG_OFFSET0      0x29    /* RW, 24-bit, OFFSET0..OFFSET7: 0x29..0x30 */
#define AD7124_REG_GAIN0        0x31    /* RW, 24-bit, GAIN0..GAIN7: 0x31..0x38 */

/* ═══════════════════════════════════════════════════════════════════
 * 通信寄存器 (每一帧 SPI 的第一个字节)
 *   [WEN=0] [R/W] [RS5..RS0]
 *   写: reg_addr        (bit6=0)
 *   读: 0x40 | reg_addr (bit6=1)
 * ═══════════════════════════════════════════════════════════════════ */
#define AD7124_COMM_WR          0x00    /* bit6=0 → 写 */
#define AD7124_COMM_RD          0x40    /* bit6=1 → 读 */
#define AD7124_COMM_RA(x)       ((x) & 0x3F)

/* ═══════════════════════════════════════════════════════════════════
 * ID 寄存器 (R, 8-bit)
 *   [7:4] DEVICE_ID        = 0x0 → AD7124-4, 0x1 → AD7124-8
 *   [3:0] SILICON_REVISION
 * 注意: Reset 值随硅版本 (手册 Rev.E 表 0x07, 官方 no-OS 默认 0x02),
 *       只校验 DEVICE_ID, 不校验 revision (以实物芯片为准)
 * ═══════════════════════════════════════════════════════════════════ */
#define AD7124_ID_DEVICE_MASK   0xF0
#define AD7124_ID_REV_MASK      0x0F
#define AD7124_ID_DEVICE_AD7124_4   0x00    /* DEVICE_ID = 0  */
#define AD7124_ID_DEVICE_AD7124_8   0x10    /* DEVICE_ID = 1  */
/* revision 随硅版本 (手册 0x07 / 官方 0x02), 不校验, 仅参考 */
#define AD7124_ID_SILICON           0x04

/* ═══════════════════════════════════════════════════════════════════
 * 状态寄存器 (R, 8-bit)
 *   [7] RDY            1=转换未完成  0=数据就绪
 *   [6] ERROR_FLAG     1=错误 (读 ERROR 寄存器后自动清除)
 *   [5] 0
 *   [4] POR_FLAG       1=上电复位发生 (读 STATUS 后自动清除)
 *   [3:0] CH_ACTIVE    当前转换通道号 (0-15)
 * ═══════════════════════════════════════════════════════════════════ */
#define AD7124_STATUS_RDY           (1 << 7)
#define AD7124_STATUS_ERROR_FLAG    (1 << 6)
#define AD7124_STATUS_POR_FLAG      (1 << 4)
#define AD7124_STATUS_CH_ACTIVE(x)  ((x) & 0x0F)

/* ═══════════════════════════════════════════════════════════════════
 * ADC_CTRL 寄存器 (RW, 16-bit, Reset=0x0000)
 *   [15:13] 0 (必须写0)
 *   [12]    DOUT_RDY_DEL    Dout_RDY 延迟
 *   [11]    CONT_READ       连续读模式
 *   [10]    DATA_STATUS     读 DATA 时附加状态字节
 *   [9]     CS_EN           片选使能 (用于 SPI 校验)
 *   [8]     REF_EN          内部基准电压使能
 *   [7:6]   POWER_MODE      00=Low 01=Mid 10/11=Full
 *   [5:2]   Mode            工作模式
 *   [1:0]   CLK_SEL         时钟源选择
 * ═══════════════════════════════════════════════════════════════════ */
#define AD7124_ADC_REF_EN           (1 << 8)
#define AD7124_ADC_POWER_MODE(x)    (((x) & 0x3) << 6)
#define AD7124_ADC_MODE(x)          (((x) & 0xF) << 2)
#define AD7124_ADC_CLK_SEL(x)       (((x) & 0x3) << 0)

#define AD7124_ADC_DATA_STATUS      (1 << 10)
#define AD7124_ADC_CONT_READ        (1 << 11)
#define AD7124_ADC_CS_EN            (1 << 9)
#define AD7124_ADC_DOUT_RDY_DEL     (1 << 12)

/* 工作模式 (ADC_CTRL Mode[3:0], bit[5:2]; 与 ADI 官方 no-OS enum ad7124_mode 一致) */
#define AD7124_MODE_CONT            0   /* 连续转换          */
#define AD7124_MODE_SINGLE          1   /* 单次转换          */
#define AD7124_MODE_STANDBY         2   /* 待机              */
#define AD7124_MODE_POWERDOWN       3   /* 掉电              */
#define AD7124_MODE_IDLE            4   /* 空闲 (默认)       */
#define AD7124_MODE_CAL_INT_Z       5   /* 内部零位校准       */
#define AD7124_MODE_CAL_INT_F       6   /* 内部满量程校准     */
#define AD7124_MODE_CAL_SYS_Z       7   /* 系统零位校准       */
#define AD7124_MODE_CAL_SYS_F       8   /* 系统满量程校准     */

/* 功耗模式 */
#define AD7124_PWR_LOW              0
#define AD7124_PWR_MID              1
#define AD7124_PWR_FULL             2

/* 时钟源 */
#define AD7124_CLK_INT              0   /* 内部 614.4kHz     */
#define AD7124_CLK_INT_OUT          1   /* 内部 + CLK 输出   */
#define AD7124_CLK_EXT              2   /* 外部 614.4kHz     */
#define AD7124_CLK_EXT_DIV          3   /* 外部/4            */

/* ═══════════════════════════════════════════════════════════════════
 * 通道寄存器 CHx_MAP (RW, 16-bit)
 *   [15]    CH_ENABLE       1=使能
 *   [14:12] SETUP           关联的配置组 (0-7)
 *   [11:10] 0 (必须写0)
 *   [9:5]   AINP[4:0]       正输入选择
 *   [4:0]   AINM[4:0]       负输入选择
 * ═══════════════════════════════════════════════════════════════════ */
#define AD7124_CH_ENABLE            (1 << 15)
#define AD7124_CH_SETUP(x)          (((x) & 0x7) << 12)
#define AD7124_CH_AINP(x)           (((x) & 0x1F) << 5)
#define AD7124_CH_AINM(x)           (((x) & 0x1F) << 0)

/* ── AINP / AINM 输入选择 (5-bit) ── */
/* 外部模拟输入 */
#define AD7124_AIN_AIN0             0
#define AD7124_AIN_AIN1             1
#define AD7124_AIN_AIN2             2
#define AD7124_AIN_AIN3             3
#define AD7124_AIN_AIN4             4
#define AD7124_AIN_AIN5             5
#define AD7124_AIN_AIN6             6
#define AD7124_AIN_AIN7             7
/* 内部通道 (AINP 有效; AINM 中 16 为 reserved) */
#define AD7124_AIN_TEMP             16      /* 温度传感器 (仅 AINP) */
#define AD7124_AIN_AVSS             17      /* AVSS */
#define AD7124_AIN_INT_REF          18      /* 内部基准 */
#define AD7124_AIN_DGND             19      /* DGND */
#define AD7124_AIN_AVDD_MON_P       20      /* (AVDD-AVSS)/6+   监测 AVDD */
#define AD7124_AIN_AVDD_MON_M       21      /* (AVDD-AVSS)/6-   */
#define AD7124_AIN_IOVDD_MON_P      22      /* (IOVDD-DGND)/6+ 监测 IOVDD */
#define AD7124_AIN_IOVDD_MON_M      23      /* (IOVDD-DGND)/6- */
#define AD7124_AIN_ALDO_MON_P       24      /* (ALDO-AVSS)/6+  监测模拟 LDO */
#define AD7124_AIN_ALDO_MON_M       25      /* (ALDO-AVSS)/6-  */
#define AD7124_AIN_DLDO_MON_P       26      /* (DLDO-DGND)/6+  监测数字 LDO */
#define AD7124_AIN_DLDO_MON_M       27      /* (DLDO-DGND)/6- */
#define AD7124_AIN_20MV_P           28      /* 20mV p-p 信号 + (用于诊断) */
#define AD7124_AIN_20MV_M           29      /* 20mV p-p 信号 - */

/* ═══════════════════════════════════════════════════════════════════
 * 配置寄存器 CFGx (RW, 16-bit, Reset=0x0860)
 *   [15:12] 0
 *   [11]    BIPOLAR         1=双极性  0=单极性
 *   [10:9]  BURNOUT         断线检测电流源 (00=off 01=0.5μA 10=2μA 11=4μA)
 *   [8]     REF_BUFP        基准正输入缓冲器
 *   [7]     REF_BUFM        基准负输入缓冲器
 *   [6]     AIN_BUFP        模拟正输入缓冲器
 *   [5]     AIN_BUFM        模拟负输入缓冲器
 *   [4:3]   REF_SEL         基准选择
 *   [2:0]   PGA             增益 (0=1, 1=2, 2=4, 3=8, ..., 7=128)
 * ═══════════════════════════════════════════════════════════════════ */
#define AD7124_CFG_BIPOLAR          (1 << 11)
#define AD7124_CFG_BURNOUT(x)       (((x) & 0x3) << 9)
#define AD7124_CFG_REF_BUFP         (1 << 8)
#define AD7124_CFG_REF_BUFM         (1 << 7)
#define AD7124_CFG_AIN_BUFP         (1 << 6)
#define AD7124_CFG_AIN_BUFM         (1 << 5)
#define AD7124_CFG_REF_SEL(x)       (((x) & 0x3) << 3)
#define AD7124_CFG_PGA(x)           (((x) & 0x7) << 0)

/* REF_SEL 值 */
#define AD7124_REF_EXT1             0   /* REFIN1± (外部基准 1) */
#define AD7124_REF_EXT2             1   /* REFIN2± (外部基准 2) */
#define AD7124_REF_INT              2   /* 内部 2.5V 基准       */
#define AD7124_REF_AVDD             3   /* AVDD-AVSS 作基准     */

/* Burnout 电流 */
#define AD7124_BURNOUT_OFF          0
#define AD7124_BURNOUT_500NA        1
#define AD7124_BURNOUT_2UA          2
#define AD7124_BURNOUT_4UA          3

/* ═══════════════════════════════════════════════════════════════════
 * 滤波器寄存器 FILTERx (RW, 24-bit, Reset=0x060180)
 *   [23:21] FILTER           滤波器类型
 *   [20]    REJ60            50/60Hz 同时抑制
 *   [19:17] POST_FILTER      后置滤波器
 *   [16]    SINGLE_CYCLE     单周期建立
 *   [15:11] 0
 *   [10:0]  FS[10:0]         输出速率: fADC = fCLK / (32 * FS), 1~2047
 *                            全功率: fCLK = 614.4kHz
 *                            FS=384 → 50Hz, FS=320 → 60Hz
 * ═══════════════════════════════════════════════════════════════════ */
#define AD7124_FILT_SINC4           0x000000
#define AD7124_FILT_SINC3           (2u << 21)
#define AD7124_FILT_FAST_SINC4      (4u << 21)
#define AD7124_FILT_FAST_SINC3      (5u << 21)
#define AD7124_FILT_REJ60           (1 << 20)
#define AD7124_FILT_POST(x)         (((x) & 0x7) << 17)
#define AD7124_FILT_SINGLE_CYCLE    (1 << 16)
#define AD7124_FILT_FS(x)           ((x) & 0x7FF)

/* ── 常用输出速率 FS 值 (全功率, SINC4) ── */
#define AD7124_FS_9P6HZ             3199    /* ~9.6 Hz  */
#define AD7124_FS_16P7HZ            1152    /* 16.7 Hz  */
#define AD7124_FS_50HZ              384     /* 50 Hz    */
#define AD7124_FS_60HZ              320     /* 60 Hz    */
#define AD7124_FS_100HZ             192     /* 100 Hz   */
#define AD7124_FS_200HZ             96      /* 200 Hz   */
#define AD7124_FS_400HZ             48      /* 400 Hz   */
#define AD7124_FS_800HZ             24      /* 800 Hz   */
#define AD7124_FS_1200HZ            16      /* 1200 Hz  */
#define AD7124_FS_2400HZ            8       /* 2400 Hz  */
#define AD7124_FS_MAX               1       /* 19.2 kHz */

/* ═══════════════════════════════════════════════════════════════════
 * ERROR 寄存器 (R, 24-bit) / ERROR_EN 寄存器 (RW, 24-bit)
 * 及 IO_CTRL1/2 省略 (本项目暂不需要 GPIO/激励电流源)
 * ═══════════════════════════════════════════════════════════════════ */
#define AD7124_ERR_ADC_SAT           (1u << 16)
#define AD7124_ERR_ADC_CONV          (1u << 17)
#define AD7124_ERR_ADC_CAL           (1u << 18)
#define AD7124_ERR_LDO_CAP           (1u << 19)
#define AD7124_ERR_AINP_OV           (1u << 15)
#define AD7124_ERR_AINP_UV           (1u << 14)
#define AD7124_ERR_AINM_OV           (1u << 13)
#define AD7124_ERR_AINM_UV           (1u << 12)
#define AD7124_ERR_REF_DET           (1u << 11)
#define AD7124_ERR_SPI_IGNORE        (1u << 6)
#define AD7124_ERR_SPI_SCLK_CNT      (1u << 5)
#define AD7124_ERR_SPI_READ          (1u << 4)
#define AD7124_ERR_SPI_WRITE         (1u << 3)
#define AD7124_ERR_SPI_CRC           (1u << 2)

/* ═══════════════════════════════════════════════════════════════════
 * 设备句柄
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    spi_if_t     *spi;           /* SPI 总线接口                    */
    GPIO_TypeDef *cs_port;       /* CS GPIO 端口                    */
    uint16_t      cs_pin;        /* CS GPIO 引脚                    */
    uint8_t       id;            /* ID 寄存器值 (上电后读取)         */
    uint8_t       adc_mode;      /* 当前转换模式 (MODE 字段)         */
    uint8_t       num_ch;        /* 实际使能的通道数 (init 写入)     */
    void        (*wait_ms)(uint32_t ms);  /* 延时回调, NULL=HAL_Delay 忙等 */
} ad7124_t;

/* ═══════════════════════════════════════════════════════════════════
 * API
 * ═══════════════════════════════════════════════════════════════════ */

/* ── 基础操作 ── */
int  ad7124_reset(ad7124_t *d);
int  ad7124_read_id(ad7124_t *d);
int  ad7124_read_reg(ad7124_t *d, uint8_t reg, uint8_t *buf, uint8_t len);
int  ad7124_write_reg(ad7124_t *d, uint8_t reg, uint8_t *buf, uint8_t len);

/* ── 初始化 (B1: 3通道 VDD1/VDD2/VDDQ, CH3 原理图空置) ── */
int  ad7124_init(ad7124_t *d);

/* ── 工作模式切换 ── */
int  ad7124_set_mode(ad7124_t *d, uint8_t mode);

/* ── 通道/配置 (setup 0-7 共用同一编号的 CFGx + FILTERx + OFFSETx + GAINx) ── */
int  ad7124_config_channel(ad7124_t *d, uint8_t ch,
                           uint8_t enable, uint8_t setup,
                           uint8_t ainp, uint8_t ainm);
int  ad7124_config_setup(ad7124_t *d, uint8_t setup,
                         uint8_t pga, uint8_t bipolar,
                         uint8_t ref_sel,
                         uint8_t filter, uint16_t fs);

/* ── 校准 ── */
int  ad7124_calibrate(ad7124_t *d, uint8_t ch);

/* ── 数据读取 ── */
int  ad7124_read_ch(ad7124_t *d, uint8_t ch, int32_t *raw);
int  ad7124_read_ch_mv(ad7124_t *d, uint8_t ch, float vref, uint8_t pga,
                       uint8_t bipolar, int32_t *mv);
int  ad7124_read_all(ad7124_t *d, int32_t *raw, uint8_t max_ch, uint8_t *got);
int  ad7124_read_all_mv(ad7124_t *d, int32_t *mv, uint8_t max_ch, uint8_t *got,
                        float vref, const uint8_t *pga, const uint8_t *bipolar);
int  ad7124_read_error(ad7124_t *d, uint32_t *err_flags);

/* ── 转换 (芯片到货后根据实际分压比调整 vref/系数) ── */
float  ad7124_raw_to_voltage(int32_t raw, float vref, uint8_t pga, uint8_t bipolar);
int32_t ad7124_raw_to_mv(int32_t raw, float vref, uint8_t pga, uint8_t bipolar);

/* ── 带前端分压换算 (B2: AIN 输入经 100R+10K 分压, divider=1.01) ──
 *   V_src = V_ADC × divider,  divider = (100+10000)/10000 = 1.01
 *   1.0 = 无分压 (与 raw_to_voltage/mv 等价) */
float  ad7124_raw_to_voltage_div(int32_t raw, float vref, uint8_t pga,
                                 uint8_t bipolar, float divider);
int32_t ad7124_raw_to_mv_div(int32_t raw, float vref, uint8_t pga,
                             uint8_t bipolar, float divider);

/* B2 三轨前端分压 (VDD1/VDD2/VDDQ 均 100R+10K) */
#define AD7124_DIV_100R_10K   1.01f

#endif
