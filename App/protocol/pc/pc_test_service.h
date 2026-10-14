/* PC测试业务：管理整轮、FUNC和SLT；复用电源、RK队列及结果灯业务接口。 */
#ifndef PC_TEST_SERVICE_H
#define PC_TEST_SERVICE_H

#include "pc_dispatch.h"
#include "pc_test_message.h"

/* 仅TCP任务调用，须先受理0106。外部已启动模式不设置电压/Boot；B1还要求真实电源ON。
 * 完整配置由正式电源编排准备，READY后才启动统计并投递算法；accepted表示
 * 请求受理，不保证算法已下发。缺少板级配置/RK适配器仍拒绝，不伪造确认。
 * start中的字符串只借用至返回；在途上下文保存静态算法目录及必要字段副本。
 */
pc_dispatch_result_t pc_test_service_start(const pc_test_start_view_t *start,
    uint32_t sequence, pc_dispatch_response_t *response);

/** 0106只建立当前连接的整轮；已有整轮/测试/维护操作时拒绝。
 * 默认常温闪烁仅为显示初值，不是B2温度状态或可触碰确认；不操作实际加热。
 */
pc_dispatch_result_t pc_test_round_begin(uint32_t sequence, pc_dispatch_response_t *response);

/** 0107在完整JSON校验之后调用；位0/1/2分别为两组LPDDR及eMCP的FAIL。
 * 原子更新三灯后关闭整轮；未入RK队列的FUNC不再投递，已入队的请求仍须等待
 * 实际结束才能解除busy，但不再交付结果。此操作不取消RK、不切电、不控温。
 */
pc_dispatch_result_t pc_test_round_end(uint32_t failed_mask,
    uint32_t sequence, pc_dispatch_response_t *response);

/** 010A的start须经专用解析器验证。仅准备电源/Boot；不向RK发送算法，
 * 不等待RK普通心跳、EDK启动或测试进度。completed之后仍占用，直到收到010B。
 */
pc_dispatch_result_t pc_test_slt_start(const pc_test_start_view_t *start,
    uint32_t sequence, pc_dispatch_response_t *response);

/** 010B已完整校验后按当前连接/site/name匹配一次；成功仅回收SLT占用。
 * 即使0107已收尾仍可接收本连接的迟到010B，但不得重开整轮或覆盖最终灯。
 * 没有执行ID，不跨TCP连接认领旧SLT，也不以相同名称宣称防重放。
 */
pc_dispatch_result_t pc_test_slt_result(const pc_slt_result_view_t *result,
    uint32_t sequence, pc_dispatch_response_t *response);

/* 非阻塞领取异步响应；1=有完整PDU，0=无，-1=组包失败，应关闭连接。
 * response=NULL仅用于无连接时回收旧测试结果；不会丢弃当前连接的响应。
 */
int pc_test_service_poll(uint32_t sequence, pc_dispatch_response_t *response);

/* 断线结束连接所属整轮并废弃结果，不取消RK、不清除在途/超时未知/SLT占用。 */
void pc_test_service_disconnected(void);
/* 1=正在准备电源、测试已入队、执行中或超时后状态未知；0=无本服务在途请求。 */
int pc_test_service_busy(void);

#endif
