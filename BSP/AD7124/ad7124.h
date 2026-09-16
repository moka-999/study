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
 * ID寄存器（R，8-bit）：B1原理图的1～24脚功能与AD7124-4 TSSOP一致。
 * ADI资料给出的有效值包括0x04（早期器件）、0x06（B grade）和
 * Rev.E复位值0x07。按有效整字节白名单校验，拒绝浮空MISO的0x00/0xFF。
 * ═══════════════════════════════════════════════════════════════════ */
#define AD7124_ID_AD7124_4_STD      0x04U
#define AD7124_ID_AD7124_4_B        0x06U
#define AD7124_ID_AD7124_4_REV_E    0x07U

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
    spi_if_t     *spi;           /* 借用SPI接口；需覆盖本句柄使用期。 */
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
/*
 * 本驱动为同步、不可重入接口；调用方负责串行访问同一SPI总线及片选。
 * 配置/读取不在ISR执行；wait_ms可让出CPU，但不会使整个API变成异步。
 * 读写缓冲由调用方持有，函数返回后可复用；len单位为字节。
 * IF_OK表示本次操作完成；ID/实测值验收仍需真实器件，Mock不代表校准。
 */
/**
 * @brief 发送64个高电平数据时钟复位ADC，等待1ms后尝试读STATUS。
 * @param[in,out] d 非NULL句柄，含有效SPI send及CS端口；wait_ms可为NULL。
 * @return 参数错误为IF_ERR；否则返回复位序列的SPI发送结果。
 * @note 当前不根据STATUS读取或POR_FLAG判定成功；缓存模式置IDLE。
 *       返回IF_OK只代表序列发送成功，不证明复位完成。
 */
int  ad7124_reset(ad7124_t *d);

/**
 * @brief 读取ID并按0x04/0x06/0x07整字节白名单校验。
 * @param[in,out] d 非NULL且SPI/CS有效；读成功后保存d->id。
 * @return IF_OK读到支持ID；IF_ERR访问失败或ID不在白名单。
 * @note ID不支持时仍保存实际读到的值，供诊断。
 */
int  ad7124_read_id(ad7124_t *d);

/**
 * @brief 在一次CS低电平事务内同步读取寄存器数据。
 * @param[in] d 非NULL、SPI transfer和CS有效的句柄。
 * @param[in] reg 寄存器地址，发送时仅保留低6位。
 * @param[out] buf 非NULL、至少len字节；失败保持原值。
 * @param[in] len 读取字节数1..4，须匹配寄存器及DATA_STATUS配置。
 * @return IF_OK成功；IF_ERR参数或SPI访问失败。
 * @note 不验证CS端口或寄存器实际宽度；CS在传输失败后也释放。
 */
int  ad7124_read_reg(ad7124_t *d, uint8_t reg, uint8_t *buf, uint8_t len);

/**
 * @brief 在一次CS低电平事务内同步写入寄存器数据。
 * @param[in] d 非NULL、SPI send和CS有效的句柄。
 * @param[in] reg 寄存器地址，仅保留低6位。
 * @param[in] buf 非NULL、至少len字节，按高字节在前提供，函数不修改。
 * @param[in] len 写入字节数1..4，调用方须匹配寄存器实际宽度。
 * @return 参数无效返回IF_ERR；否则透传SPI发送结果。
 * @note 不更新软件模式/通道缓存，也不验证寄存器内容；原始写后须保持缓存一致。
 */
int  ad7124_write_reg(ad7124_t *d, uint8_t reg, uint8_t *buf, uint8_t len);

/**
 * @brief 复位并配置三路DDR采样，使用内部2.5V基准和连续模式。
 * @param[in,out] d 非NULL、SPI和CS已绑定的句柄；成功后num_ch为3。
 * @return IF_OK初始化操作完成；IF_ERR参数、ID或任一配置访问失败。
 * @note 同步调用，失败可能已更新部分硬件/缓存，不能按就绪设备使用。
 *       默认CH0..2对应AIN0/1、2/3、4/5；保持既有正端缓冲、负端AGND配置。
 */
int  ad7124_init(ad7124_t *d);

/**
 * @brief 写入固定ADC_CTRL基底及MODE，并在成功后更新模式缓存。
 * @param[in,out] d 非NULL且SPI/CS有效的句柄。
 * @param[in] mode AD7124_MODE_*模式，调用方须提供支持值。
 * @return IF_OK写入成功；IF_ERR参数无效，或透传SPI错误。
 * @note 本实现整寄存器写入内部时钟、全功率、内部基准和DATA_STATUS，
 *       不保留其他ADC_CTRL选项；模式按低4位编码，不等待转换/校准结束。
 */
int  ad7124_set_mode(ad7124_t *d, uint8_t mode);

/**
 * @brief 配置一个硬件通道的使能、setup及正负输入映射。
 * @param[in] d SPI/CS有效的设备句柄。
 * @param[in] ch 通道编号0..15。
 * @param[in] enable 0禁用，非0使能。
 * @param[in] setup 共用配置组0..7。
 * @param[in] ainp 正输入选择，低5位编码，调用方保证器件支持。
 * @param[in] ainm 负输入选择，低5位编码，调用方保证接线/器件支持。
 * @return IF_OK写入成功；IF_ERR编号/接口无效，或底层SPI错误。
 * @note 不更新d->num_ch；批量读取仍要求连续编号和软件通道数与硬件一致。
 */
int  ad7124_config_channel(ad7124_t *d, uint8_t ch,
                           uint8_t enable, uint8_t setup,
                           uint8_t ainp, uint8_t ainm);

/**
 * @brief 依次写CFGx和FILTERx，保留OFFSET/GAIN校准寄存器。
 * @param[in] d SPI/CS有效的设备句柄。
 * @param[in] setup 配置组0..7，被关联到该组的通道共用。
 * @param[in] pga 增益编码0..7，对应1..128倍。
 * @param[in] bipolar 0单极性，非0双极性。
 * @param[in] ref_sel 基准编码0..3。
 * @param[in] filter 当前为8位原始值，直接与FS位或合并；不能承载高位滤波宏。
 * @param[in] fs 速率字段，调用方应传1..2047；当前仅拒绝大于2047。
 * @return IF_OK两项写入完成；IF_ERR参数无效，或透传底层访问错误。
 * @note 第二次写失败时CFG可能已生效；本通用接口打开所有输入/基准缓冲，
 *       不等同于init中针对B1负端接地的配置。
 */
int  ad7124_config_setup(ad7124_t *d, uint8_t setup,
                         uint8_t pga, uint8_t bipolar,
                         uint8_t ref_sel,
                         uint8_t filter, uint16_t fs);

/**
 * @brief 尝试执行单通道内部满量程及零位校准，再恢复连续模式。
 * @param[in,out] d 非NULL、已配置的句柄，SPI/CS须有效。
 * @param[in] ch 校准通道0..7；调用方须确保更高通道未启用。
 * @return ch大于7返回IF_ERR；当前其余路径返回IF_OK。
 * @note 当前未检查保存/配置/恢复寄存器的返回值，轮询耗尽也未报超时，
 *       因此IF_OK不能证明校准成功或原通道状态已恢复。不得依此通过硬件验收。
 */
int  ad7124_calibrate(ad7124_t *d, uint8_t ch);

/**
 * @brief 等待数据就绪并读取单通道24位原始码。
 * @param[in] d 非NULL且已配置DATA_STATUS的设备，SPI/CS有效。
 * @param[in] ch 目标硬件通道；连续模式按返回状态匹配，其他模式接受当前结果。
 * @param[out] raw 非NULL输出；成功为0..0xFFFFFF，超时置0，IO失败可能保留旧值。
 * @return IF_OK读到结果；IF_ERR访问失败；IF_TIMEOUT轮询耗尽或未找到目标通道。
 * @note 未校验d/raw空指针。连续模式最多尝试8次转换，每次最多500轮1ms等待，
 *       另有SPI耗时；结果不作符号扩展，双极性使用offset-binary换算。
 */
int  ad7124_read_ch(ad7124_t *d, uint8_t ch, int32_t *raw);

/**
 * @brief 读取单通道并换算为整数毫伏，不补偿前端分压。
 * @param[in] d 非NULL、已配置DATA_STATUS的有效设备。
 * @param[in] ch 通道编号，匹配规则同ad7124_read_ch。
 * @param[in] vref 实际基准电压，单位V。
 * @param[in] pga 增益编码0..7，须与硬件一致。
 * @param[in] bipolar 0单极性，非0双极性，须与硬件一致。
 * @param[out] mv 非NULL，成功写入mV，读取失败保持原值。
 * @return 透传read_ch结果：IF_OK、IF_ERR或IF_TIMEOUT。
 * @note 未校验输出指针；等待行为同read_ch。
 */
int  ad7124_read_ch_mv(ad7124_t *d, uint8_t ch, float vref, uint8_t pga,
                       uint8_t bipolar, int32_t *mv);

/**
 * @brief 轮询DATA_STATUS并按硬件通道号收集原始码。
 * @param[in] d 非NULL有效设备；启用通道应连续为0..num_ch-1，num_ch不超过8。
 * @param[out] raw 非NULL、至少max_ch项，按实际通道号写入，未收到项保持原值。
 * @param[in] max_ch 输出项数，内部取其与num_ch较小值。
 * @param[out] got 可为NULL；实际收到的不同通道数，不是连续有效前缀长度。
 * @return 收到至少一通道即IF_OK；一个都未收到则IF_TIMEOUT（包括IO失败）。
 * @note 不检查d/raw空指针；完整快照须有足够容量且got等于num_ch。
 *       最多num_ch * 4次转换尝试，每次最多500轮1ms等待，另有SPI耗时。
 */
int  ad7124_read_all(ad7124_t *d, int32_t *raw, uint8_t max_ch, uint8_t *got);

/**
 * @brief 批量读取并按每通道增益/极性换算为毫伏。
 * @param[in] d 非NULL有效设备，通道配置前提同read_all。
 * @param[out] mv 非NULL、至少max_ch项；成功路径按0..*got-1写入。
 * @param[in] max_ch 输出项数，应至少为d->num_ch且不超过临时数组容量16。
 * @param[out] got 必须非NULL；返回实际收集数。
 * @param[in] vref 实际基准电压，单位V。
 * @param[in] pga 每通道增益编码数组，至少max_ch项；NULL默认全部0。
 * @param[in] bipolar 每通道极性数组，至少max_ch项；NULL默认全部单极性。
 * @return 透传read_all结果，IF_OK不保证收齐通道。
 * @note 当前按数量换算前缀，但read_all可能稀疏收集；缺通道时可能读取未
 *       初始化的局部raw项，输出不能使用。此为现有实现缺陷，不能仅靠检查
 *       返回值解决，后续应修正有效通道跟踪/换算路径。
 */
int  ad7124_read_all_mv(ad7124_t *d, int32_t *mv, uint8_t max_ch, uint8_t *got,
                        float vref, const uint8_t *pga, const uint8_t *bipolar);

/**
 * @brief 读取24位ERROR寄存器，供器件诊断。
 * @param[in] d SPI/CS有效的设备句柄。
 * @param[out] err_flags 非NULL，成功写入错误位，失败保持原值。
 * @return IF_OK读取成功；IF_ERR读取失败。
 * @note 未校验err_flags空指针；读取具有清除器件错误状态的副作用。
 */
int  ad7124_read_error(ad7124_t *d, uint32_t *err_flags);

/**
 * @brief 将24位ADC码换算为ADC输入电压。
 * @param[in] raw 0..0xFFFFFF原始码，双极性为offset-binary，不能先符号扩展。
 * @param[in] vref 实际基准电压，单位V，须有限且大于0。
 * @param[in] pga 增益编码0..7，实现按低3位索引。
 * @param[in] bipolar 0单极性，非0双极性。
 * @return 电压，单位V。
 * @note 纯计算，不检查输入范围；调用方保证换算结果在返回类型可表示范围内。
 */
float  ad7124_raw_to_voltage(int32_t raw, float vref, uint8_t pga, uint8_t bipolar);

/**
 * @brief 将24位ADC码换算为ADC输入电压。
 * @param[in] raw 0..0xFFFFFF原始码，双极性为offset-binary，不能先符号扩展。
 * @param[in] vref 实际基准电压，单位V，须有限且大于0。
 * @param[in] pga 增益编码0..7，实现按低3位索引。
 * @param[in] bipolar 0单极性，非0双极性。
 * @return 电压，单位mV；浮点到整数向零截断。
 * @note 纯计算，不检查输入范围；调用方保证换算结果在返回类型可表示范围内。
 */
int32_t ad7124_raw_to_mv(int32_t raw, float vref, uint8_t pga, uint8_t bipolar);

/**
 * @brief 将24位ADC码换算为分压前电源电压。
 * @param[in] raw 0..0xFFFFFF原始码，双极性为offset-binary，不能先符号扩展。
 * @param[in] vref 实际基准电压，单位V，须有限且大于0。
 * @param[in] pga 增益编码0..7，实现按低3位索引。
 * @param[in] bipolar 0单极性，非0双极性。
 * @param[in] divider 前端分压还原倍率，须有限且大于0；1为无分压，B1为1.01。
 * @return 电压，单位V。
 * @note 纯计算，不检查输入范围；调用方保证换算结果在返回类型可表示范围内。
 */
float  ad7124_raw_to_voltage_div(int32_t raw, float vref, uint8_t pga,
                                 uint8_t bipolar, float divider);

/**
 * @brief 将24位ADC码换算为分压前电源电压。
 * @param[in] raw 0..0xFFFFFF原始码，双极性为offset-binary，不能先符号扩展。
 * @param[in] vref 实际基准电压，单位V，须有限且大于0。
 * @param[in] pga 增益编码0..7，实现按低3位索引。
 * @param[in] bipolar 0单极性，非0双极性。
 * @param[in] divider 前端分压还原倍率，须有限且大于0；1为无分压，B1为1.01。
 * @return 电压，单位mV；浮点到整数向零截断。
 * @note 纯计算，不检查输入范围；调用方保证换算结果在返回类型可表示范围内。
 */
int32_t ad7124_raw_to_mv_div(int32_t raw, float vref, uint8_t pga,
                             uint8_t bipolar, float divider);

/* B1三轨前端分压（VDD1/VDD2/VDDQ均为100R+10K） */
#define AD7124_DIV_100R_10K   1.01f

#endif
