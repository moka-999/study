/* B1本地日志出口：USART1输出二选一，USART2双向转发RK调试字符，UART4协议不参与。 */
#ifndef B1_LOG_CONSOLE_H
#define B1_LOG_CONSOLE_H

#include <stddef.h>

/* 编译默认值由CMake生成：0=MCU，1=RK；每次启动恢复，不持久化运行时选择。 */
#ifndef B1_LOG_DEFAULT_SOURCE
#define B1_LOG_DEFAULT_SOURCE 0
#endif
#if B1_LOG_DEFAULT_SOURCE != 0 && B1_LOG_DEFAULT_SOURCE != 1
#error "B1_LOG_DEFAULT_SOURCE must be 0 (MCU) or 1 (RK)"
#endif

/* 默认任务在uart_start_all()后调用一次；不初始化硬件，不访问电源或Flash。 */
void log_console_start(void);

/* 默认任务每轮调用一次；MCU模式解释log mcu/log rk/status加CR、LF或CRLF。
 * RK模式逐字节转发输入，仅截获行首完整log mcu加换行；保留CR/LF及控制字符。
 * 行首退出前缀暂存到失配/换行；退格编辑前缀，Ctrl+C丢弃前缀并转发。
 * 每轮最多消费控制台和RK日志各一个接收块，无数据时立即返回。
 * RK发送拥塞时暂存一块、不再取新输入；RX断流丢弃至换行并报告，不能保证无损。
 * UART故障恢复沿用路由层HAL超时；不建立新任务，不触碰UART4协议。 */
void log_console_process(void);

/* 任务上下文写MCU日志，返回已消费字节数；RK模式或控制回复待发时消费但不输出。
 * 尚未启动、ISR、参数错误返回0；出口满时返回已入队前缀且累计丢字节。
 * 不等待TX空间，不保证可靠送达；用于诊断，不能承载业务协议或故障处置。
 * printf的stdout/stderr也使用此出口；请勿再直接写huart1绕过来源选择。 */
size_t log_console_write(const char *text, size_t length);

/* 默认任务消费一条status请求：1有请求、0无；重复请求合并，控制回复先入队。
 * 不含业务查询；未选择MCU时拒绝查询，切换RK时清除尚未消费的请求。 */
int log_console_take_status_request(void);
/* 默认任务发送1..128字节状态分块：1完整入队，0出口忙可稍后重试，
 * -1来源切换/未启动/非法参数/ISR，调用方应取消报告；不阻塞、不分配内存。
 * 与普通日志的“隐藏仍算已消费”不同，保证报告不会在等待控制回复时跳过数据。 */
int log_console_write_status(const char *text, size_t length);

#endif
