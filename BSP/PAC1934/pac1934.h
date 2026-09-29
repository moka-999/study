/*
 * ===================================================================
 * PAC1934T — 4 通道电流/功率监视器 (Microchip DS20005850)
 * ===================================================================
 *
 * 硬件（B1 20260920-2134版原理图 P22）：
 *   逻辑I2C4(PH4/PH5软件开漏，非STM32 I2C4 AF)，7位地址0x16
 *   （总线上写地址字节0x2C、读地址字节0x2D）
 *   SENSE1 = VCC_1V8_S3_LPDDR4X  (R7268，0.02Ω) → VDD1 电流
 *   SENSE2 = VDD2_DDR_S3        (R7269，0.02Ω) → VDD2 电流
 *   SENSE3 = VDDQ_DDR           (R7270，0.02Ω) → VDDQ 电流
 *   SENSE4 未用 → 禁用
 *
 * 换算 (手册 Equation 4-1/4-4/4-7, unipolar):
 *   Vbus  = VBUS_reg  × 32V / 2^16          (LSB = 488.3µV)
 *   FSC   = 100mV / Rsense
 *   I     = FSC × VSENSE_reg / 2^16         (LSB = FSC/65536)
 *   P     = PowerFSR × VPOWER_reg / 2^28
 *   PowerFSR = (100mV/Rsense) × 32V
 *
 * 初始化: 身份检查 → 通道/单极性/速率配置 → REFRESH → 等待并回读ACT
 * 读取:    REFRESH_V 后实际至少1ms，核对ACT/LAT再读取测量结果
 *
 * B1绑定: i2c4_if使用PH4/PH5软件开漏GPIO直连U204；
 *           真实IDD、零点和增益待首板标定。
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
#define PAC1934_REG_CTRL_LAT      0x24   /* 当前锁存结果所用的CTRL配置，不作为报警清除接口 */
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
#define PAC1934_RSENSE_B1_UOHM      20000U /* B1三路均为20mΩ；仅板级参数，不改变通用换算公式 */
#define PAC1934_CH_MASK_B1_RAILS    0x07   /* 启用CH1-3，关闭CH4 */
#define PAC1934_CH_MASK_ALL          0x0F   /* bit0-3 = ch1-4 使能位 */

typedef void (*pac1934_wait_ms_fn)(uint32_t ms);

/* 最近一次调用的诊断原因；不是DDR测试结果，不改变IF_OK/IF_ERR返回合同。 */
typedef enum {
    PAC1934_ERROR_NONE = 0,
    PAC1934_ERROR_PARAM,   /* 空指针、参数或句柄尚未就绪。 */
    PAC1934_ERROR_IO,      /* I2C读写失败；active_mask清零，须重新初始化。 */
    PAC1934_ERROR_ID,      /* 产品/厂商不匹配；不能把浮空数据当作器件在线。 */
    PAC1934_ERROR_CONFIG,  /* ACT或LAT与连续单极性配置不符；须重新初始化。 */
    PAC1934_ERROR_DATA,    /* 功率寄存器保留位非法；须重新初始化。 */
    PAC1934_ERROR_RANGE,   /* 测量码到达量程上沿，疑似饱和；仅本次结果无效。 */
} pac1934_error_t;

typedef struct {
    i2c_if_t *bus;            /* 借用总线实例，不内置互斥锁。 */
    uint8_t addr;             /* 未左移7位地址，B1为0x16。 */
    uint32_t rsense_uohm;     /* 采样电阻µΩ；B1显式20000，V2测试配置10000。 */
    uint8_t active_mask;      /* bit0..3对应CH1..4；0表示未就绪。 */
    pac1934_wait_ms_fn wait_ms; /* 回调须保证实际至少等待ms；任务上下文调用。 */
    uint8_t sample_rate;      /* 初始化成功后的速率编码0..3，不由外部直接修改。 */
    pac1934_error_t last_error; /* IF_ERR时读取；随后调用可能覆盖。 */
} pac1934_t;

typedef struct {
    uint16_t vbus;            /* 16位总线电压原始码。 */
    uint16_t vsense;          /* 16位采样电阻压差码，不是电流。 */
    uint32_t vpower;          /* 已从线上bit31..4右移对齐的28位功率码。 */
} pac1934_chan_t;

typedef struct {
    uint32_t vbus_mv;         /* 电压mV，向下取整。 */
    uint32_t current_ma;      /* 兼容现有PC/统计的整数mA，不在此修改业务格式。 */
    uint32_t power_mw;        /* 功率mW，向下取整。 */
    uint64_t current_ua;      /* 保留低电流信息的µA值；不是1µA精度承诺。 */
} pac1934_measurement_t;

/*
 * 全部接口为同步调用：上层须串行保护完整刷新/等待/读取事务。
 * 总线实例和等待回调由调用方持有，使用期间必须有效；驱动不分配内存。
 * 两个批量接口的输出必须至少4项，即使只启用3路；成功时禁用通道清零。
 * 读取/换算失败均不改变输出；只能在IF_OK后使用，不能误用上次旧值。
 * 不在驱动中定义IDD窗口、PASS/FAIL、温控或RK状态。
 */
/**
 * @brief 检查身份，配置连续单极性采样并核对实际生效配置。
 * @param[in,out] d 借用总线和等待回调；rsense_uohm为0兼容填10000µΩ。
 * @param[in] ch_mask 非0的低4位掩码，bit0..3对应CH1..4。
 * @param[in] sample_rate 0/1/2/3分别为1024/256/64/8 SPS的寄存器配置。
 * @return IF_OK就绪；IF_ERR失败且active_mask=0，具体原因见last_error。
 * @note 总线IO外另等待20ms启动、126ms旧周期/配置切换及1/4/16/125ms新周期。
 *       SLOW外部引脚仍可覆盖实际速率，等真板确认；这不是IDD业务等待窗口。
 */
int pac1934_init(pac1934_t *d, uint8_t ch_mask, uint8_t sample_rate);

/**
 * @brief REFRESH_V锁存后至少等待1ms，核对ACT/LAT并读取全部启用通道。
 * @param[in,out] d 已初始化设备；通信/配置失信时会清除active_mask。
 * @param[out] out 非NULL、至少4项，按CH1..4排列，成功时禁用通道清零。
 * @return IF_OK完整快照可用；IF_ERR时out保持原值，原因见last_error。
 * @note 不复位累加器；调用方负责整个刷新/等待/读取事务的串行保护。
 */
int pac1934_read_all(pac1934_t *d, pac1934_chan_t *out);

/**
 * @brief 核对ACT/LAT后读取一个已启用通道；不主动刷新或等待。
 * @param[in,out] d 已初始化设备，错误诊断和就绪状态可能更新。
 * @param[in] ch 零起始索引0..3，须在active_mask中启用。
 * @param[out] out 非NULL单通道输出；原始码，不是工程单位。
 * @return IF_OK成功；IF_ERR时out保持原值，原因见last_error。
 * @note 调用方必须先锁存、等够时间，并持有完整事务锁。
 */
int pac1934_read_ch(pac1934_t *d, uint8_t ch, pac1934_chan_t *out);

/**
 * @brief 一次刷新、读取并换算全部启用通道，沿用read_all的同步事务合同。
 * @param[in,out] d 已初始化设备，rsense_uohm须为实际非零分流电阻。
 * @param[out] out 非NULL、至少4项，输出mV/mA/mW及µA，成功时禁用项清零。
 * @return IF_OK完整结果可用；IF_ERR时out保持原值，原因见last_error。
 */
int pac1934_measure_all(pac1934_t *d, pac1934_measurement_t *out);

/* 以下纯整数换算不访问硬件；电阻单位µΩ；除零或非法功率码返回0而非错误码。 */
uint32_t pac1934_vbus_mv(uint16_t vbus_reg);
uint64_t pac1934_current_ua(uint16_t vsense_reg, uint32_t rsense_uohm);
uint32_t pac1934_current_ma(uint16_t vsense_reg, uint32_t rsense_uohm);
/* vpower_reg须是已右移对齐的28位码；禁止直接传入I2C四字节值。 */
uint32_t pac1934_power_mw(uint32_t vpower_reg, uint32_t rsense_uohm);

#endif
