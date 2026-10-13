/* B1本地日志出口：USART1输出二选一，USART2只接收RK日志，UART4协议不参与。 */
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

/* 默认任务每轮调用一次，接收log mcu/log rk加CR、LF或CRLF；仅本地解释命令。
 * 每轮最多消费控制台和RK日志各一个接收块，无数据时立即返回。
 * UART故障恢复沿用路由层HAL超时；不建立新任务，不转发输入给RK。 */
void log_console_process(void);

/* 任务上下文写MCU日志，返回已消费字节数；RK模式或控制回复待发时消费但不输出。
 * 尚未启动、ISR、参数错误返回0；出口满时返回已入队前缀且累计丢字节。
 * 不等待TX空间，不保证可靠送达；用于诊断，不能承载业务协议或故障处置。
 * printf的stdout/stderr也使用此出口；请勿再直接写huart1绕过来源选择。 */
size_t log_console_write(const char *text, size_t length);

#endif
