/* B1调试出口：USART1与TCP8889独立选源，复用USART2调试，UART4协议不参与。 */
#ifndef B1_LOG_CONSOLE_H
#define B1_LOG_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

/* 编译默认值由CMake生成：0=MCU，1=RK；每次启动恢复，不持久化运行时选择。 */
#ifndef B1_LOG_DEFAULT_SOURCE
#define B1_LOG_DEFAULT_SOURCE 0
#endif
#if B1_LOG_DEFAULT_SOURCE != 0 && B1_LOG_DEFAULT_SOURCE != 1
#error "B1_LOG_DEFAULT_SOURCE must be 0 (MCU) or 1 (RK)"
#endif

/* 默认任务在uart_start_all()后调用一次；不初始化硬件，不访问电源或Flash。 */
void log_console_start(void);

/* 010C按站点状态放行两路控制台的电源、网络交接和验灯命令；关站仍可选日志和看status。 */
void log_console_set_site_enabled(uint8_t enabled);

/* 默认任务每轮调用一次；MCU模式解释log mcu/log rk/status及led验灯命令，
 * start rk/end rk只登记请求，另由唯一TCP业务上下文推进电源和Boot。
 * led 0|1|2|all off|pass|fail|normal|hot仅用于空闲验灯，单组命令关闭其他组；
 * 也可用00/01/10/11固定两路电平：左FAIL引脚、右PASS引脚，0低1高，11持续双亮。
 * 只更新结果灯，不改变PC轮次或加热，正式PC/B2事件仍可覆盖，结束用led all off。
 * 本地命令须加CR、LF或CRLF。
 * RK模式逐字节转发输入，仅截获行首完整log mcu加换行；保留CR/LF及控制字符。
 * 行首退出前缀暂存到失配/换行；退格编辑前缀，Ctrl+C丢弃前缀并转发。
 * 每轮最多消费控制台和RK日志各一个接收块，无数据时立即返回。
 * RK发送拥塞时暂存一块、不再取新输入；RX断流丢弃至换行并报告，不能保证无损。
 * 网络与串口分别保留半行，RK输入由先选择RK的一端独占；另一端仅观察。
 * 退出/断线释放输入权；观察端重新log mcu、log rk后可申请控制。
 * UART故障恢复沿用路由层HAL超时；不建立新任务，不触碰UART4协议。 */
void log_console_process(void);

typedef enum {
    LOG_CONSOLE_POWER_NONE = 0,
    LOG_CONSOLE_POWER_START_RK, /* start rk：LPDDR4X标称1800/1100/600mV、fspiM1冷启动。 */
    LOG_CONSOLE_POWER_END_RK    /* end rk：沿用现有正常下电流程。 */
} log_console_power_request_t;

/* 两条命令仅在MCU模式识别，须完整换行；单槽不覆盖、不自动重试。
 * 唯一TCP任务领取一次后须调用complete；不要求PC连接，不在串口解析临界区访问电源。
 * 领取返回NONE表示无请求；测试或电源忙由业务层拒绝，完成仅表示本地电源/Boot动作。
 */
log_console_power_request_t log_console_take_power_request(void);
/* 唯一TCP任务反馈：NULL成功，其它为静态错误名；复制到固定缓冲后由默认任务发送。
 * 切换日志来源不取消动作；回复在UART拥塞时保留，完整入队后才接受下一条电源命令。
 */
void log_console_complete_power_request(const char *error);

/* 任务上下文写MCU日志，各出口按自身来源过滤；返回串口已消费字节数。
 * 尚未启动、ISR、参数错误返回0；串口满时返回已入队前缀且累计丢字节，网络独立尝试。
 * 不等待TX空间，不保证可靠送达；用于诊断，不能承载业务协议或故障处置。
 * printf的stdout/stderr也使用此出口；请勿再直接写huart1绕过来源选择。 */
size_t log_console_write(const char *text, size_t length);

/* 任务上下文：任一已连接出口选择MCU返回1。单条含换行最多256字节，各出口整条入队或丢弃。 */
#define LOG_CONSOLE_RECORD_SIZE 256U
int log_console_mcu_selected(void);
void log_console_write_record(const char *text, size_t length);
uint32_t log_console_dropped_bytes(void);

/* 默认任务消费一条status请求：1有请求、0无；各出口重复请求合并，控制回复先入队。
 * 报告只返回请求端；不含业务查询，断线/换代/切换RK后取消该端报告。 */
int log_console_take_status_request(void);
/* 默认任务发送1..128字节状态分块：1完整入队，0出口忙可稍后重试，
 * -1来源切换/未启动/非法参数/ISR，调用方应取消报告；不阻塞、不分配内存。
 * 与普通日志的“隐藏仍算已消费”不同，保证报告不会在等待控制回复时跳过数据。
 * 初次自动报告只发串口；网络端需输入status，不回放历史报告。 */
int log_console_write_status(const char *text, size_t length);

#endif
