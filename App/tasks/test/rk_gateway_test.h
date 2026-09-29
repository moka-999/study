/* 仅隔离联调镜像使用：模拟上电/ready条件，不参与正式PC业务。 */
#ifndef RK_GATEWAY_TEST_H
#define RK_GATEWAY_TEST_H
#include "pc_dispatch.h"
#include "pc_test_message.h"
pc_dispatch_result_t rk_gateway_test_start(const pc_test_start_view_t *start,
    uint32_t sequence, pc_dispatch_response_t *response);
/* 返回1有异步响应、0无、-1组帧失败；由TCP任务分配发送Seq并发送。 */
int rk_gateway_test_poll(uint32_t sequence, pc_dispatch_response_t *response);
void rk_gateway_test_disconnect(void);
#endif
