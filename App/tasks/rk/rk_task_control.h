/* TCP与RK任务间的停止及启动回读协调；UART收发由RK任务执行。 */
#ifndef RK_TASK_CONTROL_H
#define RK_TASK_CONTROL_H

#include <stdint.h>

/* 唯一TCP任务登记停止；RK任务暂停发送，真实OFF后自行清理请求、结果及解析器。 */
void rk_task_stop_begin(void);
/* 1表示已收到停止请求或正等待停止收尾，禁止再登记调试查询。 */
int rk_task_stopping(void);
/* 1表示已确认OFF、清理完成且UART空闲；0仍在等待，不代表测试PASS。 */
int rk_task_stop_ready(void);
/* TCP完成0104收尾后解除门禁；须先取得ready=1。 */
void rk_task_stop_end(void);

/* 每次物理冷启动布防一次。非零表示先完成时序写入，再开始回读。 */
void rk_timing_readback_arm(int defer_until_settings);
/* 电源编排拉起RK前安装本次手动频率；-1表示使用RK默认值。 */
void rk_link_manual_frequency(int64_t value);
/* 本次启动的最后一条设置已入UART，或已决定跳过设置，可以回读。 */
void rk_timing_readback_settings_done(void);
/* 返回1表示本次上电已收到与本轮心跳序号匹配的SPL普通心跳，掉电后清零。 */
int rk_timing_startup_ready(void);
/* 1表示本轮启动扫描准备/正在读回，暂缓投递算法；手动查询不占用此门禁。 */
int rk_timing_readback_busy(void);
/* 本次启动终止或EDK提前接管串口，停止等待；直到下次布防才会再次查询。 */
void rk_timing_readback_cancel(void);

/* 调试口登记单项或 all（全部普通时序）。NULL 表示受理，其他返回值为静态错误字符串。 */
const char *rk_task_check_timing(const char *name);
/* 调试口的普通时序写入，成功受理后由RK任务发帧并回读；门禁持续到回读结束。 */
const char *rk_task_set_timing(uint16_t command, uint32_t value);
int rk_task_set_pending(void);

#endif
