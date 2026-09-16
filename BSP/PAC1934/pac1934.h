/*
 * ===================================================================
 * PAC1934T — 4 通道电流/功率监视器 (Microchip DS20005850)
 * ===================================================================
 *
 * 硬件（B1/MB360 原理图 P22）：
 *   逻辑I2C4(PH4/PH5软件开漏，非STM32 I2C4 AF)，7位地址0x16
 *   （总线上写地址字节0x2C、读地址字节0x2D）
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
#define PAC1934_ADDR_B1             0x16U  /* 未左移7位地址；底层发送时统一左移1位 */
#define PAC1934_RSENSE_B1_UOHM      10000U /* 三路均为0.01Ω */
#define PAC1934_CH_MASK_B1_RAILS    0x07   /* 启用CH1-3，关闭CH4 */
#define PAC1934_CH_MASK_ALL          0x0F   /* bit0-3 = ch1-4 使能位 */

/* CHANNEL_DIS(0x1C) 的通道关闭位实际位于bit7..4，不是bit3..0。 */
#define PAC1934_CH_DISABLE_BITS(mask)  ((uint8_t)((~(mask) & PAC1934_CH_MASK_ALL) << 4))

typedef void (*pac1934_wait_ms_fn)(uint32_t ms);

typedef struct {
    i2c_if_t *bus;       /* I2C 总线接口 (i2c4_if) */
    uint8_t   addr;      /* 未左移的7位I2C地址（B1为0x16） */
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

/*
 * 同步任务接口，不内置总线锁；同一设备/总线须由上层串行访问。
 * addr使用未左移7位地址。批量读取/换算的输出须容纳4个通道，
 * 即使只启用前三路，也不能传3项数组；禁用通道输出清零。
 * wait_ms供刷新后的等待使用，回调和总线实例均由调用方保证生命周期。
 */
/**
 * @brief 校验产品/厂商ID，配置启用通道和采样率并启动转换。
 * @param[in,out] d 非NULL设备句柄；rsense_uohm为0时填默认10000微欧。
 * @param[in] ch_mask 非0的低4位掩码，bit0..3对应CH1..4。
 * @param[in] sample_rate 0/1/2/3分别选择1024/256/64/8 SPS。
 * @return IF_OK全部操作成功并保存active_mask；IF_ERR参数、ID或总线失败。
 * @note 失败可能已改变部分寄存器，旧active_mask不会自动清除；不能仅据
 *       旧掩码判定本次初始化成功。
 */
int  pac1934_init(pac1934_t *d, uint8_t ch_mask, uint8_t sample_rate);

/**
 * @brief REFRESH_V锁存后等待1ms并读取全部启用通道。
 * @param[in,out] d 非NULL已初始化句柄，wait_ms回调必须有效。
 * @param[out] ch 非NULL、至少4项，按CH1..4索引，成功时禁用通道清零。
 * @return IF_OK全部启用通道读取完成；IF_ERR参数或总线失败。
 * @note 同步调用；失败时输出可能只更新前缀，不是完整快照。REFRESH_V不
 *       复位累加器，调用方负责整个刷新/等待/读取事务的串行化。
 */
int  pac1934_read_all(pac1934_t *d, pac1934_chan_t *ch);

/**
 * @brief 读取一个启用通道的VBUS/VSENSE/VPOWER原始寄存器。
 * @param[in] d 非NULL已初始化句柄。
 * @param[in] ch 零起始索引0..3，须在active_mask中启用。
 * @param[out] out 非NULL输出，仅IF_OK时三个字段都有效。
 * @return IF_OK成功；IF_ERR参数、通道未启用或总线失败。
 * @note 不主动刷新；由调用方先锁存并等待。失败时部分字段可能已经更新。
 */
int  pac1934_read_ch(pac1934_t *d, uint8_t ch, pac1934_chan_t *out);

/**
 * @brief 刷新、读取并换算全部启用通道的电压/电流/功率。
 * @param[in,out] d 已初始化设备，含有效wait_ms和实际分流电阻参数。
 * @param[out] out 非NULL、至少4项，单位依次mV/mA/mW，禁用通道成功时清零。
 * @return IF_OK完整结果可用；IF_ERR参数或读取失败，此时out保持原值。
 * @note 同步事务；单极性换算，驱动不产生RK侧IDD测试激励。
 */
int  pac1934_measure_all(pac1934_t *d, pac1934_measurement_t *out);

/**
 * @brief 将单极性VBUS原始值换算为毫伏。
 * @param[in] vbus_reg 16位原始寄存器值。
 * @return 向下取整的电压mV，不访问硬件。
 */
uint32_t pac1934_vbus_mv(uint16_t vbus_reg);

/**
 * @brief 将单极性VSENSE原始值换算为毫安。
 * @param[in] vsense_reg 16位原始采样值。
 * @param[in] rsense_uohm 实际分流电阻，单位微欧。
 * @return 整数电流mA；电阻为0返回0，此值不能区分参数错误和零电流。
 */
uint32_t pac1934_current_ma(uint16_t vsense_reg, uint32_t rsense_uohm);

/**
 * @brief 按单极性28位量程将VPOWER原始值换算为毫瓦。
 * @param[in] vpower_reg 原始功率码，调用方须保证符合28位量程。
 * @param[in] rsense_uohm 实际分流电阻，单位微欧。
 * @return 整数功率mW；电阻为0返回0，不是独立错误码。
 */
uint32_t pac1934_power_mw(uint32_t vpower_reg, uint32_t rsense_uohm);

#endif
