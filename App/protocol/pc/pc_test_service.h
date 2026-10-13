/* PC功能测试业务：调用电源编排与RK队列，不直接访问socket、GPIO或总线。 */
#ifndef PC_TEST_SERVICE_H
#define PC_TEST_SERVICE_H

#include "pc_dispatch.h"
#include "pc_test_message.h"

/* 仅TCP任务调用。外部已启动模式不设置电压/Boot；B1还要求真实电源ON。
 * 完整配置由正式电源编排准备，READY后才启动统计并投递算法；accepted表示
 * 请求受理，不保证算法已下发。缺少板级配置/RK适配器仍拒绝，不伪造确认。
 * start中的字符串只借用至返回；在途上下文保存静态算法目录及必要字段副本。
 */
pc_dispatch_result_t pc_test_service_start(const pc_test_start_view_t *start,
    uint32_t sequence, pc_dispatch_response_t *response);

/* 非阻塞领取异步响应；1=有完整PDU，0=无，-1=组包失败，应关闭连接。
 * response=NULL仅用于无连接时回收旧测试结果；不会丢弃当前连接的响应。
 */
int pc_test_service_poll(uint32_t sequence, pc_dispatch_response_t *response);

/* 断线撤销结果的连接归属，不取消RK、不清除在途/超时未知状态。 */
void pc_test_service_disconnected(void);
/* 1=正在准备电源、测试已入队、执行中或超时后状态未知；0=无本服务在途请求。 */
int pc_test_service_busy(void);

#endif
