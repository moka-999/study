/*
 * Apollo V2电源状态机（用于B1业务的软件验证）
 *
 * 管理三组硬件使能：4.0V 系统输入、3.3V 系统电源和 DDR 三轨总使能。
 * B1独立工程保留其PC6/PC7真实控制及PG11可选总闸，不照搬V2的第三阶段延时。
 * 本模块只负责顺序和故障收敛，不决定尚未经过真板确认的具体毫秒值。
 *
 * 单实例、任务上下文接口，不内置状态锁。当前monitor_task的事务锁统一
 * 串行化PC配置、电源推进和采样；业务使用monitor_*入口，不直接并发调用。
 * get_state只提供瞬时值，不能据此保证后续耗时操作期间电源状态不变。
 */
#ifndef PWR_CTRL_H
#define PWR_CTRL_H

#include <stdint.h>
#include "ddr_power.h"

/* PC6 与 PC7 的先后关系必须由最终硬件评审结果显式选择。 */
typedef enum {
    PWR_CTRL_RAIL_ORDER_4V0_FIRST = 0,
    PWR_CTRL_RAIL_ORDER_3V3_FIRST,
} pwr_ctrl_rail_order_t;

/*
 * 所有延时均为可标定参数，单位为毫秒。
 * V2 模拟值不能直接复制到 B1 量产配置；真板必须用示波器重新确认。
 */
typedef struct {
    pwr_ctrl_rail_order_t rail_order;
    uint32_t first_to_second_delay_ms;
    uint32_t second_to_ddr_delay_ms;
    uint32_t ddr_enable_to_config_delay_ms;
    uint32_t ddr_to_system_off_delay_ms;
    uint32_t system_rail_off_spacing_ms;
} pwr_ctrl_config_t;

typedef enum {
    PWR_CTRL_STATE_OFF = 0,
    PWR_CTRL_STATE_WAIT_FIRST_RAIL,
    PWR_CTRL_STATE_WAIT_SECOND_RAIL,
    PWR_CTRL_STATE_WAIT_DDR_READY,
    PWR_CTRL_STATE_ON,
    PWR_CTRL_STATE_WAIT_DDR_OFF,
    PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF,
    PWR_CTRL_STATE_FAULT,
} pwr_ctrl_state_t;

typedef enum {
    PWR_CTRL_OK = 0,
    PWR_CTRL_ERR_PARAM = -1,
    PWR_CTRL_ERR_NOT_CONFIGURED = -2,
    PWR_CTRL_ERR_BUSY = -3,
    PWR_CTRL_ERR_FAULT = -4,
    PWR_CTRL_ERR_RESOURCE_BUSY = -5,
} pwr_ctrl_result_t;

/**
 * @brief 初始化使能GPIO并强制所有电源关闭。
 * @param[in] config 时序配置，复制保存；NULL或非法配置使上电保持未配置状态。
 * @note 启动期调用；不报告配置失败，可通过is_configured查询。时序必须来自
 *       硬件评审/标定，函数仅检查配置形式，不证明电气安全。
 */
void pwr_ctrl_init(const pwr_ctrl_config_t *config);

/**
 * @brief 在OFF状态复制系统电源时序配置。
 * @param[in] config 非NULL配置，所有延时单位为ms，调用后可释放。
 * @return OK配置成功；ERR_PARAM空指针/顺序无效；ERR_BUSY当前不是OFF。
 * @note 失败保留原配置；不校验延时是否经过硬件批准。
 */
pwr_ctrl_result_t pwr_ctrl_configure(const pwr_ctrl_config_t *config);

/**
 * @brief 请求上电，立即开启第一轨并交给process继续推进。
 * @param[in] now_ms 与process共用的单调毫秒时基，允许uint32_t自然回绕。
 * @return OK请求受理或已ON；ERR_NOT_CONFIGURED无配置；ERR_FAULT故障；
 *         ERR_BUSY处于其他过渡状态。
 * @note OK不表示各轨已经稳定；本函数不等待完整上电。沿用已选训练目标，
 *       默认/重新选择类型后为标称值，不沿用运行期拉偏。
 */
pwr_ctrl_result_t pwr_ctrl_request_on(uint32_t now_ms);

/**
 * @brief 仅在OFF时复制明确的训练目标并请求上电，之后由process完成TPS写入。
 * @param[in] type LPDDR4或LPDDR4X。
 * @param[in] mv 非NULL、3项VDD1/VDD2/VDDQ目标mV；目标复制后可释放。
 * @param[in] now_ms 与process共用的毫秒时基。
 * @return OK已受理；ERR_PARAM目标无效；ERR_NOT_CONFIGURED时序未配置；
 *         ERR_FAULT故障；ERR_BUSY非OFF（包含已ON，不静默实施运行期调压）。
 * @note 业务先审核被测器件安全范围，并完成正常掉电、放电和Boot选择；
 *       此入口不自动关机/重启，不负责RK-ready，不是新PC协议入口。
 *       “训练前赶得上”仅为开发预设，需B1验证总线、调度和模拟稳定的最坏时间。
 */
pwr_ctrl_result_t pwr_ctrl_request_training_on(
    ddr_memory_type_t type, const uint16_t mv[DDR_RAIL_COUNT], uint32_t now_ms);

/**
 * @brief 请求下电，ON时开始逆序关闭，其他过渡态立即全部关断。
 * @param[in] now_ms 与process共用的毫秒时基。
 * @return OK已OFF或请求受理；ERR_FAULT保持故障并再次关断全部使能。
 * @note 正常业务调用前必须取得RK安全掉电确认，本层不实现握手。ON下电需
 *       后续process推进；在下电过渡态重复调用也会立即全部关断。
 */
pwr_ctrl_result_t pwr_ctrl_request_off(uint32_t now_ms);

/**
 * @brief 在OFF状态选择下次上电使用的内存标称电压组。
 * @param[in] type DDR_MEMORY_LPDDR4或DDR_MEMORY_LPDDR4X。
 * @return OK成功；ERR_BUSY非OFF；ERR_PARAM类型无效。
 * @note 只改变软件目标，不执行I2C写入。
 */
pwr_ctrl_result_t pwr_ctrl_select_memory_type(ddr_memory_type_t type);

/**
 * @brief 在ON状态同步写入三轨相对标称值的拉偏档位。
 * @param[in] steps 非NULL、至少3项，依次VDD1/VDD2/VDDQ，每档10/5/5mV。
 * @return OK写入成功；ERR_PARAM目标不可编码；ERR_BUSY非ON；ERR_FAULT已故障
 *         或任一轨写失败（立即关断全部使能并进入FAULT）。
 * @note 调用方须确保RK已完成DDR初始化；当前未实现RK ready强制门控。
 *       可编码范围不等于DDR安全范围；串行任务上下文调用，I2C可能阻塞。
 */
pwr_ctrl_result_t pwr_ctrl_apply_ddr_steps(const int16_t steps[DDR_RAIL_COUNT]);

/**
 * @brief 周期推进电源状态机，每次最多执行当前阶段的一次迁移。
 * @param[in] now_ms 与请求入口共用的单调毫秒时基。
 * @note 轨间等待不休眠；DDR初始化仍是同步I2C，可能被总线超时阻塞。
 *       初始化失败转FAULT并全关；通过get_state观察结果。普通业务使用
 *       monitor_power_process，以便与采样/配置共用事务锁。
 */
void pwr_ctrl_process(uint32_t now_ms);

/**
 * @brief 仅在FAULT时再次关断所有使能并回到OFF。
 * @note 其他状态不操作，不能用作紧急关机；保留时序配置，不自动重新上电。
 */
void pwr_ctrl_clear_fault(void);

/**
 * @brief 查询当前软件电源状态。
 * @return 瞬时状态，不保证后续操作期间不变化，也不代表实测电压。
 */
pwr_ctrl_state_t pwr_ctrl_get_state(void);

/**
 * @brief 查询是否已经接受过有效的时序配置。
 * @return 1已配置；0未配置。该标志不表示真板时序已验收。
 */
uint8_t pwr_ctrl_is_configured(void);

#endif
