/* PC功能测试业务：连接JSON命令与RK队列，不操作socket或电源硬件。 */
#ifndef PC_TEST_SERVICE_H
#define PC_TEST_SERVICE_H

#include "pc_dispatch.h"
#include "pc_test_message.h"

/* 仅TCP任务调用。外部已启动模式不设置电压/Boot；返回accepted仅表示入队。
 * B1还要求真实电源ON，并启动后台实测统计；自动重启/训练请求仍不开放。
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
/* 1=测试已入队、执行中或超时后状态未知；0=可提交下一测试。 */
int pc_test_service_busy(void);

#endif
