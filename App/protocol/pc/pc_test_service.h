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
 * Freq在启动查询0x35中回复，目标变化通过既有冷启动流程处理；外部模式不接受非负Freq。
 * 普通时序在准备完成后逐项发送，不等待RK回应；发送失败停止，不自动重发。
 * accepted/completed不表示参数已生效；FUNC统计在算法入队前开始；IDD统计以进入帧实际发完为基准。
 */
pc_dispatch_result_t pc_test_service_start(uint16_t request_command, const pc_test_start_view_t *start,
    uint32_t sequence, pc_dispatch_response_t *response);

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

/** 010A的start须经专用解析器验证。不向RK发送算法或检查EDK/测试进度。
 * 默认时序保持原有电源/Boot准备；非负Freq须完成本轮查询回复，非空普通时序须等
 * 本轮SPL普通心跳后发送；completed只表示本地准备/发送完成，仍占用至010B。
 */
pc_dispatch_result_t pc_test_slt_start(const pc_test_start_view_t *start,
    uint32_t sequence, pc_dispatch_response_t *response);

/** 010B已完整校验后按当前连接/site/name匹配一次；成功仅回收SLT占用。
 * 即使0107已收尾仍可接收本连接的迟到010B，但不得重开整轮或覆盖最终灯。
 * 没有执行ID，不跨TCP连接认领旧SLT，也不以相同名称宣称防重放。
 */
pc_dispatch_result_t pc_test_slt_result(const pc_slt_result_view_t *result,
    uint32_t sequence, pc_dispatch_response_t *response);

/* 非阻塞领取异步响应；1=有完整PDU，0=无响应或RK结果等待组包重试。
 * RK结果只保留一份，组包失败不重发算法、不解除busy、不主动断线；准备阶段
 * 的其他致命错误仍可返回-1。返回1只代表组包成功，不保证TCP已交付PC。
 * response=NULL仅用于无连接时回收旧测试结果；不丢弃当前连接的待组包结果。
 */
int pc_test_service_poll(uint32_t sequence, pc_dispatch_response_t *response);

/* 断线结束连接所属整轮并废弃结果，不取消RK、不清除在途/超时未知/SLT占用。 */
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
