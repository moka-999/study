/* PC测试业务：管理整轮、FUNC/PARA和SLT；复用电源、RK队列及结果灯接口。 */
#ifndef PC_TEST_SERVICE_H
#define PC_TEST_SERVICE_H

#include "pc_dispatch.h"
#include "pc_test_message.h"

/* 仅TCP任务调用，须先受理0106。外部已启动模式不设置电压/Boot；B1还要求真实电源ON。
 * 完整配置由正式电源编排准备，READY后才启动统计并投递算法；accepted表示
 * 请求受理，不保证算法已下发。缺少板级配置/RK适配器仍拒绝，不伪造确认。
 * start中的字符串/JSON只借用至返回；在途上下文保存静态算法目录及必要字段副本。
 * request_command仅允许0103/FUNC或0108/PARA，名称须属于对应RK算法类别。
 * PARA发送101A进入，实际发完后等5ms并采集5s，再发送退出；不等待RK应答。
 * 0109两颗默认PASS/resultDesc=null，电流使用逐点统计；这是开发约定，不是标准判限。
 * board_rk_config.h分别控制普通时序和Freq；关闭后仍校验PC请求，原文副本不改。
 * Freq启用时按本轮目标回复0x35、变化走冷启动且外部模式拒绝非负值；关闭统一使用-1。
 * 普通时序启用时在准备完成后逐项发送，不等待RK回应；发送失败停止，不自动重发。
 * accepted/completed不表示参数已生效；FUNC统计在算法入队前开始；IDD统计以进入帧实际发完为基准。
 */
pc_dispatch_result_t pc_test_service_start(uint16_t request_command, const pc_test_start_view_t *start,
    uint32_t sequence, pc_dispatch_response_t *response);

/* 0104：NULL表示开始停止；否则返回静态错误名。仅TCP上下文调用，无需先0106。
 * 不等待RK/EDK回应，沿用既有下电；真实OFF及队列/串口清理后才由poll报completed。
 * 废弃当前未交付结果，保留整轮；失败保留占用，可再次0104重试，不跨连接补响应。 */
const char *pc_test_service_stop(void);

/** 0106只建立当前连接的整轮；已有整轮/测试/维护操作时拒绝。
 * 使用B2已知模式显示；尚未知的镜像灭灯，不推断为常温；不操作实际加热。
 */
pc_dispatch_result_t pc_test_round_begin(uint32_t sequence, pc_dispatch_response_t *response);

/** 仅TCP上下文处理B2模式通知时调用：mask位0..2，modes为对应0常温/1高温。
 * 有效整轮内更新被通知的灯；轮外只由B2服务保存缓存，保留0107最终灯。
 * 1成功（含轮外无操作），0输入非法/灯驱动未就绪；不控温、不修改算法状态。
 */
int pc_test_round_update_modes(uint8_t mask, const uint8_t modes[3]);

/** 0107在完整JSON校验之后调用；位0/1/2分别为两组LPDDR及eMCP的FAIL。
 * 原子更新三灯后关闭整轮；未入RK队列的FUNC/PARA不再投递，已入队的请求仍须等待
 * 实际结束才能解除busy，但不再交付结果。此操作不取消RK、不切电、不控温。
 */
pc_dispatch_result_t pc_test_round_end(uint32_t failed_mask,
    uint32_t sequence, pc_dispatch_response_t *response);

/** 010A的start须经专用解析器验证，沿用FUNC/PARA的整轮及电源/Boot/时序准备。
 * 沿用同一组普通时序/Freq开关。Freq仅问才回，关闭回复-1；普通时序启用才等心跳并逐项发送。
 * completed只表示本地准备/发送完成，随后自动003E/EDK/PXE；不表示测试通过。
 * RELEASE最终应答发完后自动OFF并恢复串口，才解除执行占用；不等待PC结果解析。
 * 正式流程独立于串口net/start调试入口，仅支持保持PC连接的共存配置。
 */
pc_dispatch_result_t pc_test_slt_start(const pc_test_start_view_t *start,
    uint32_t sequence, pc_dispatch_response_t *response);

/* 非阻塞领取异步响应；1=有完整PDU，0=无响应或RK结果等待组包重试。
 * RK结果只保留一份，组包失败不重发算法、不解除busy、不主动断线；准备阶段
 * 的其他致命错误仍可返回-1。返回1只代表组包成功，不保证TCP已交付PC。
 * response=NULL仅用于无连接时回收旧测试结果；不丢弃当前连接的待组包结果。
 */
int pc_test_service_poll(uint32_t sequence, pc_dispatch_response_t *response);

/* 断线结束整轮且不向新连接交付旧结果；SLT仍等RELEASE后自动完成物理收尾。 */
void pc_test_service_disconnected(void);
/* 1=准备电源、执行中、结果待组包或超时后状态未知；0=无本服务在途请求。 */
int pc_test_service_busy(void);

/** 读取最近一次已受理FUNC/PARA/SLT的时序对象，只供TCP任务本地诊断。
 * 返回只读借用指针：NULL=尚未保存或该请求未提供/null；{}与空字符串原样保留。
 * 不包含硬件生效状态，不写W25、不生成新响应字段、不合并上一请求的参数。
 * 下一次成功受理开始请求会替换并释放旧对象；拒绝、结果结束或断线不覆盖副本。
 * 调用者不得修改/释放或跨下一次受理保留该指针，也不得从其他任务无锁读取。
 */
const cJSON *pc_test_service_timing_parameters(void);

#endif
