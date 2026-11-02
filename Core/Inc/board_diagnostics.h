/* B1首板诊断：只观察初始化过程和运行快照，不执行硬件测试或业务命令。 */
#ifndef BOARD_DIAGNOSTICS_H
#define BOARD_DIAGNOSTICS_H

/* 仅main调度前调用；stage须为长期有效的短字符串，供串口及SWD定位失败阶段。 */
void board_diagnostics_boot_stage(const char *stage);
/* USART1初始化成功后启用早期输出；只在编译默认MCU模式下打印。 */
void board_diagnostics_boot_console_ready(void);
/* 创建内核对象前关闭轮询出口，此后USART1完全归日志路由，不再直接发送。 */
void board_diagnostics_boot_finish(void);
/* Error_Handler停机前尝试报告最近阶段；UART未就绪/运行期/中断屏蔽时不发送。 */
void board_diagnostics_boot_error(void);
/* 仅默认任务在lwIP初始化后周期调用；启动自动报告一次，之后由status触发。
 * 使用RAM快照与GPIO输出锁存值；不采样、不探测器件、不改电源/IP或协议状态。 */
void board_diagnostics_process(void);

#endif
