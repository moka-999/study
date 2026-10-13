/*
 * ===================================================================
 * TPS62868/TPS62869 — 4A/6A 同步降压 I2C 可调 (TI SLVSFS3B)
 * ===================================================================
 *
 * 硬件（B1 原理图 20260920-2134；V2仅模拟该三轨配置）：
 *   U4025 = I2C1 → VCC_1V8_S3_LPDDR4X (VDD1, 1.8V)
 *   U4026 = I2C2 → VDD2_DDR_S3       (VDD2, 1.1V)
 *   U4027 = I2C4 → VDDQ_DDR          (VDDQ, 0.6V)
 *   EN 各由10K上拉至VCC4V0_SYS；到PG11的R4073/R4081/R4086均为NC
 *   VIN = VCC4V0_SYS，因此PC6开启4V后三颗TPS按VSET启动值自动启动
 *
 * 器件版本 (A 版, VSET/PG 电阻启动配置):
 *   U4025: TPS628692A, 56.2k, factor=2, 启动1.60V, 地址0x49
 *   U4026: TPS628681A, 86.6k, factor=1, 启动0.90V, 地址0x41
 *   U4027: TPS628681A, 15.4k, factor=1, 启动0.50V, 地址0x4F
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
 * B1绑定: 三颗分别挂 i2c1_if/i2c2_if/逻辑i2c4_if；
 *           PH4/PH5由软件开漏GPIO实现，真实VOUT待首板测量。
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

/* A 版地址由 VSET/PG 电阻决定，三路分别定义，禁止共用单一地址。 */
#define TPS62868_ADDR_VDD1      0x49
#define TPS62868_ADDR_VDD2      0x41
#define TPS62868_ADDR_VDDQ      0x4F

/* 电压因子 (x10 整数避免浮点): x0=5, x1=10, x2=20 */
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
    uint8_t   addr;      /* I2C 7-bit 地址，由 VSET/PG 电阻决定       */
    uint8_t   factor_x10; /* 电压因子 x10 (5/10/20, 按 BOM 料号)     */
    uint8_t   ctl_cache; /* CONTROL 软件镜像 (write-only 寄存器, 默认 0x6F) */
} tps62868_t;

/*
 * 同步任务接口：bus/设备句柄由调用方持有，同一CONTROL缓存不得并发修改。
 * addr为未左移7位地址；电压单位mV。寄存器写成功不保证负载电压已稳定，
 * 更不表示该电压对DDR安全；上电/故障策略仍属于pwr_ctrl/ddr_power。
 */
/**
 * @brief 扫描0x40..0x4F中的I2C应答地址，最多保存max个。
 * @param[in] bus 非NULL且具有is_ready回调的同步I2C接口。
 * @param[out] addrs 至少max字节；max为0时允许NULL。
 * @param[in] max 输出容量，不额外扫描已无法存储的地址。
 * @return 保存的地址数；0也可能是参数无效，不区分扫描失败与无设备。
 * @note 仅探测ACK，不验证应答器件是否为TPS62868。
 */
int  tps62868_scan(i2c_if_t *bus, uint8_t *addrs, uint8_t max);

/**
 * @brief 同步读取一个TPS寄存器字节。
 * @param[in] d 非NULL、read_reg回调及7位地址有效的设备。
 * @param[in] reg 寄存器地址，调用方保证可读。
 * @param[out] val 非NULL输出，只有IF_OK时有效。
 * @return 参数错误为IF_ERR；否则透传总线结果。
 * @note 读取STATUS有读后清除副作用，不是无副作用探测。
 */
int  tps62868_read_reg (tps62868_t *d, uint8_t reg, uint8_t *val);

/**
 * @brief 同步写入一个TPS寄存器字节。
 * @param[in] d 非NULL、write_reg回调及7位地址有效的设备。
 * @param[in] reg 寄存器地址，调用方保证可写。
 * @param[in] val 原始寄存器值。
 * @return 参数错误为IF_ERR；否则透传总线结果。
 * @note 原始写不更新ctl_cache；直接写CONTROL后调用方须维护缓存一致性。
 */
int  tps62868_write_reg(tps62868_t *d, uint8_t reg, uint8_t val);

/**
 * @brief 将目标电压精确编码后写VOUT1，VID为低时生效。
 * @param[in] d 非NULL句柄，factor_x10须按BOM提供5/10/20。
 * @param[in] mv 目标毫伏，须能按器件倍率和5mV基础步进精确表示。
 * @return IF_OK写成功；IF_ERR参数/编码范围错误，或透传底层总线错误。
 * @note 不量化截断目标；寄存器可写不代表DDR安全或输出已经稳定。
 */
int  tps62868_set_vout1(tps62868_t *d, uint16_t mv);

/**
 * @brief 将目标电压精确编码后写VOUT2，VID为高时生效。
 * @param[in] d 非NULL句柄，factor_x10须按BOM提供5/10/20。
 * @param[in] mv 目标毫伏，须能按器件倍率和5mV基础步进精确表示。
 * @return IF_OK写成功；IF_ERR参数/编码范围错误，或透传底层总线错误。
 * @note 不量化截断目标；寄存器可写不代表DDR安全或输出已经稳定。
 */
int  tps62868_set_vout2(tps62868_t *d, uint16_t mv);

/**
 * @brief 用CONTROL软件镜像置位软件使能位。
 * @param[in,out] d 非NULL设备句柄，调用方串行保护ctl_cache。
 * @return IF_OK写入并更新缓存；IF_ERR参数/总线失败。
 * @note ctl_cache为0时先加载默认0x6F，此初始化即使后续写失败也会保留。
 *       不驱动PG11，也不证明物理电源已完成动作。
 */
int  tps62868_enable (tps62868_t *d);

/**
 * @brief 用CONTROL软件镜像清除软件使能位。
 * @param[in,out] d 非NULL设备句柄，调用方串行保护ctl_cache。
 * @return IF_OK写入并更新缓存；IF_ERR参数/总线失败。
 * @note ctl_cache为0时先加载默认0x6F，此初始化即使后续写失败也会保留。
 *       不驱动PG11，也不证明物理电源已完成动作。
 */
int  tps62868_disable(tps62868_t *d);

/**
 * @brief 修改CONTROL的输出放电使能位。
 * @param[in,out] d 非NULL句柄，缓存更新规则同tps62868_enable。
 * @param[in] on 0关闭，非0开启输出放电。
 * @return IF_OK写入成功；IF_ERR参数或总线失败。
 * @note 只配置功能，不主动关闭输出，调用方须串行保护CONTROL镜像。
 */
int  tps62868_set_discharge(tps62868_t *d, uint8_t on);

/**
 * @brief 修改CONTROL的电压斜坡速率。
 * @param[in,out] d 非NULL句柄，缓存更新规则同tps62868_enable。
 * @param[in] ramp 0/1/2/3对应20/10/5/1mV每微秒。
 * @return IF_OK写入成功；IF_ERR参数或总线失败。
 */
int  tps62868_set_ramp     (tps62868_t *d, uint8_t ramp);

/**
 * @brief 读取并由器件清除STATUS事件标志。
 * @param[in] d 有效设备句柄。
 * @param[out] flags 非NULL，热预警/HICCUP/UVLO等原始位；仅IF_OK有效。
 * @return 透传read_reg结果，IF_OK成功。
 * @note 清零的状态不能替代电源健康判定，多调用者必须协调读取。
 */
int  tps62868_read_status (tps62868_t *d, uint8_t *flags);

/**
 * @brief 写CONTROL复位位并在写成功后恢复软件镜像默认值。
 * @param[in,out] d 非NULL有效设备。
 * @return 透传寄存器写结果，IF_OK时ctl_cache恢复0x6F。
 * @note 会复位器件配置；不重新应用DDR目标，不等待实测电压稳定。
 */
int  tps62868_reset_regs  (tps62868_t *d);

#endif
