/*
 * ===================================================================
 * DDR 电源管理 — 三路 TPS62868 (VDD1/VDD2/VDDQ) 统一控制
 * ===================================================================
 *
 * B1/MB360 原理图（20260811）：
 *   rail 0 = VDD1  VCC_1V8_S3_LPDDR4X  1.8V  TPS62868 U4025  I2C1 (PB7/8)
 *   rail 1 = VDD2  VDD2_DDR_S3        1.1V  TPS62868 U4026  I2C2 (B1: PF0/1)
 *   rail 2 = VDDQ  VDDQ_DDR           0.6V  TPS62868 U4027  PH4/PH5软件I2C
 *
 * 料号 (A 版, VSET/PG 电阻选择地址和启动档位):
 *   VDD1 = TPS628692A, 56.2k, 0x49, 启动1.60V  code=(1800/2-400)/5=100
 *   VDD2 = TPS628681A, 86.8k(TI 86.6k档), 0x41, 启动0.90V  code=(1100-400)/5=140
 *   VDDQ = x1A, 15.4k, 0x4F, 启动0.50V  code=(600-400)/5=40
 *   以上是预期贴装；0831图面U4025仍为249k，需核实已改为56.2k。
 *   启动值、训练目标和模拟爬升波形须分别验收，不能据大小关系保证无过压。
 *
 * 上电流程: PC6产生4V → TPS由EN硬件上拉自动启动 → ddr_power_init()
 *           清STATUS并从VSET启动值直接调整到本次训练目标（默认标称值）。
 * 拉偏:     相对各自标称值独立调压，VDD1每档10mV，VDD2/VDDQ每档5mV。
 * 本模块由pwr_ctrl串行调用；目标缓存不是实际测得的电压。
 *
 * B1总线: VDD1=I2C1，VDD2=I2C2，VDDQ=PH4/PH5软件开漏I2C。
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
 * TPS寄存器的物理可写范围。VDD1使用×2器件，原生分辨率为10mV；
 * VDD2/VDDQ使用×1器件，原生分辨率为5mV。上层steps直接使用各轨原生步进。
 * 这些上限是“芯片能写”而不是“DDR安全”上限，极端值可能永久损坏被测器件。
 */
#define DDR_VDD1_STEP_MV         10
#define DDR_VDDX_STEP_MV         5
#define DDR_VDD1_WRITABLE_MIN_MV 800
#define DDR_VDD1_WRITABLE_MAX_MV 3350
#define DDR_VDDX_WRITABLE_MIN_MV 400
#define DDR_VDDX_WRITABLE_MAX_MV 1675

/*
 * 被测器件类型只影响VDDQ标称值：LPDDR4为1.1V，LPDDR4X为0.6V。
 * VDD1/VDD2分别保持1.8V/1.1V。
 */
typedef enum {
    DDR_MEMORY_LPDDR4 = 0,
    DDR_MEMORY_LPDDR4X,
} ddr_memory_type_t;

/**
 * @brief 绑定三轨总线，清STATUS并直接写入保存的训练目标及最快ramp。
 * @return IF_OK全部总线操作成功；IF_ERR首个错误后立即返回，前面的轨可能已配置。
 * @note 须在设备可访问时串行调用；不控制GPIO，不保证电压稳定或训练成功。
 *       失败由pwr_ctrl关断，成功才更新目标缓存；每次重建CONTROL镜像。
 *       运行期拉偏不影响下次启动目标；未显式配置时使用对应类型标称值。
 */
int  ddr_power_init(void);

/**
 * @brief 选择内存类型，将训练目标及软件目标缓存重置为对应标称值。
 * @param[in] type DDR_MEMORY_LPDDR4或DDR_MEMORY_LPDDR4X。
 * @return IF_OK成功；IF_ERR类型超出支持范围。
 * @note 不写硬件、不检查电源状态；调用方须保证OFF，业务使用pwr_ctrl入口。
 */
int  ddr_power_select_memory_type(ddr_memory_type_t type);

/**
 * @brief 复制本次冷启动的训练目标，不访问硬件，不更新已写入电压缓存。
 * @param[in] type LPDDR4或LPDDR4X，决定后续原生档位拉偏的标称零点。
 * @param[in] mv 非NULL、3项，依次为VDD1/VDD2/VDDQ，单位mV；调用后可释放。
 * @return IF_OK已保存；IF_ERR类型或任一目标不可精确编码，原配置保持不变。
 * @note 控制层必须保证OFF并串行调用；业务使用monitor_request_training_power_on。
 *       这里只检查TPS编码范围，不替代被测料号的安全范围/组合/上电波形审核。
 *       待定的PC开始测试字段不能直接裸接本接口，须先完成业务安全校验。
 */
int ddr_power_configure_training(ddr_memory_type_t type,
                                 const uint16_t mv[DDR_RAIL_COUNT]);

/**
 * @brief 按各轨原生步进计算标称值加档位后的目标，检查能否精确编码。
 * @param[in] type DDR_MEMORY_LPDDR4或DDR_MEMORY_LPDDR4X。
 * @param[in] steps 非NULL、至少3项，顺序为VDD1/VDD2/VDDQ，每档分别10/5/5mV。
 *                 0为标称值，正数升压、负数降压，不在当前电压上累加。
 * @param[out] mv 非NULL、至少3项，单位mV；失败时可能已部分或全部写入。
 * @return IF_OK三个目标可写；IF_ERR参数、算术范围或编码步进不合法。
 * @note 纯计算，不写硬件；TPS可写范围不代表DDR安全范围。
 */
int  ddr_power_build_step_targets(ddr_memory_type_t type,
                                  const int16_t steps[DDR_RAIL_COUNT],
                                  uint16_t mv[DDR_RAIL_COUNT]);

/**
 * @brief 按当前内存类型计算并同步应用三轨原生档位。
 * @param[in] steps 非NULL、至少3项，相对标称值的有符号档位。
 * @return IF_OK成功；IF_ERR目标无效或调压失败。
 * @note 本层不判断ON/DDR初始化完成、不自动断电；业务须通过pwr_ctrl调用。
 */
int  ddr_power_apply_steps(const int16_t steps[DDR_RAIL_COUNT]);

/**
 * @brief 按选轨掩码同步写入目标电压，跳过未变化的轨。
 * @param[in] rail_mask 非0，bit0/1/2分别为VDD1/VDD2/VDDQ，不允许其他位。
 * @param[in] mv 非NULL、至少3项；按轨索引取目标mV，未选轨保持原目标。
 * @return IF_OK所需写入完成；IF_ERR参数/目标无效或I2C失败。
 * @note 多器件写入不能原子完成；失败时尝试逆序回退，但回退失败未上报，
 *       不保证恢复原电压。调用方负责串行化、电源状态和故障关断。
 */
int  ddr_power_set(uint8_t rail_mask, const uint16_t *mv);

/**
 * @brief 读取一轨TPS状态，器件STATUS具有读后清除副作用。
 * @param[in] rail 轨索引0..2。
 * @param[out] flags 非NULL状态字节；仅IF_OK时有效。
 * @return IF_OK读取成功；参数/总线错误返回IF_ERR，其他底层错误向上传递。
 */
int  ddr_power_read_status(uint8_t rail, uint8_t *flags);

/**
 * @brief 查询软件保存的某轨目标电压。
 * @param[in] rail 轨索引0..2。
 * @return 目标mV；非法轨返回0。不是ADC实测值，失败回退后不可用来证明电压。
 */
uint16_t ddr_power_get_mv(uint8_t rail);

/** 读取下次冷启动训练目标mV；非法轨返回0。与运行期拉偏缓存分开，不是实测值。 */
uint16_t ddr_power_get_training_mv(uint8_t rail);

/**
 * @brief 查询当前选定的内存类型。
 * @return 当前软件配置，影响下一次初始化及拉偏零点。
 */
ddr_memory_type_t ddr_power_get_memory_type(void);

#endif
