/* 正式0104的跨任务停止请求；不新增RK线协议，也不从TCP任务操作UART缓存。 */
#ifndef RK_TASK_CONTROL_H
#define RK_TASK_CONTROL_H

/* 唯一TCP任务登记停止；RK任务暂停发送，真实OFF后自行清理请求、结果及解析器。 */
void rk_task_stop_begin(void);
/* 1表示已确认OFF、清理完成且UART空闲；0仍在等待，不代表测试PASS。 */
int rk_task_stop_ready(void);
/* TCP完成0104收尾后解除门禁；须先取得ready=1。 */
void rk_task_stop_end(void);

#endif
