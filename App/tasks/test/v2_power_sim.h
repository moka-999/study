/* V2正式TCP任务中的电源/测量联调替身；不创建任务，不访问器件，不模拟RK结果。 */
#ifndef V2_POWER_SIM_H
#define V2_POWER_SIM_H

#include "monitor_task.h"
#include "pc_test_message.h"

#if defined(B1_RK_UART)
#error "V2 power simulation must not be enabled on B1"
#endif

/* 所有入口仅由唯一TCP任务串行调用。真实monitor/pwr_ctrl状态保持独立。 */
pwr_ctrl_state_t v2_power_sim_state(void);
uint32_t v2_power_sim_cycles(void); /* 仅虚拟重启计数，MCU复位后清零。 */
/* on使用当前毫秒时基，立即置虚拟ON并返回OK；off立即置OFF并取消运行统计。 */
pwr_ctrl_result_t v2_power_sim_on(uint32_t now_ms);
void v2_power_sim_off(void);
void v2_power_sim_restart(void); /* 调用方确认虚拟电源ON后使用，不发送RK复位。 */
/* Boot仅OFF可设置，非法模式返回ERR_PARAM，非OFF返回ERR_STATE；不写DAC。 */
monitor_boot_result_t v2_power_sim_set_boot(boot_mode_t mode);
/* 未配置为ERR_NOT_SET；成功写出模式及0..1800mV目标值，均非硬件回读。 */
monitor_boot_result_t v2_power_sim_get_boot(boot_mode_t *mode, uint16_t *mv);
/* steps为3轨20mV档位；OFF须全零，ON须同类型。非法参数ERR_PARAM、状态ERR_BUSY。 */
pwr_ctrl_result_t v2_power_sim_set_ddr(ddr_memory_type_t type, const int16_t steps[3]);
int v2_power_sim_latest(monitor_snapshot_t *out); /* 只复制，不产生新样本。 */
void v2_power_sim_poll(void); /* 约500ms生成一个样本；积压时不补造历史样本。 */

/* 仅在请求已成功进入真实RK队列后调用。start必须来自严格PC解析器。
 * 新受理清统计；RK完成时complete=1冻结，通信失败/超时complete=0取消。
 * 断开PC不结束测试，也不停止统计；没有RK结果不能调用complete=1。
 */
void v2_power_sim_test_begin(const pc_test_start_view_t *start);
void v2_power_sim_test_end(int complete);

/* 0602追加statistics；0105替换唯一结果的measurementData，保留字段顺序。 */
int v2_power_sim_add_statistics(cJSON *body);
int v2_power_sim_add_measurement(cJSON *body);

#endif
