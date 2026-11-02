# FreeRTOS任务目录

- `pc/`：维护PC TCP连接、PC帧接收和响应发送。PC/MCU分别维护u32发送`Seq`，不回显请求序号。
- `rk/`：维护MCU-RK串口、TLV、独立u64 `Seq`、单个在途请求及最终结果交付；测试不自动重发。
  `rk_link.h`声明共享队列及序号分配入口，计数器仅在实现内维护；`rk_task.c`统一处理收包、请求/结果和每秒心跳，不再创建独立心跳任务。每轮最多取8个接收块，再推进事务、心跳及任务存活上报；B1未上电时不发送。
- `common/`：LED、采样监控等不专属于PC或RK链路的正式任务。
- `test/`：W25Q256自检、驱动Mock、逻辑分析仪波形等隔离验证任务。它们只能由测试构建开关创建，不得被当作生产业务入口。

PC与RK通过`pc_test_service`交换结构化数据，禁止原始帧透传或混用Seq。`rk_link_request/receive`使用`rkRequestQueue/rkResponseQueue`，cookie仅作内部关联。V2正式0103支持外部已启动模式（power/bootMode均null、reboot=false），不依赖模拟开关；带电压/启动要求的完整B1流程仍受门禁约束。`test/rk_gateway_test.c`只服务旧隔离镜像，不能与正式业务同时消费RK结果队列。

结果等待默认无限，显式非零timeoutMs才启用算法时限；通信帧间超时照常运行。网络断开不取消RK，旧测试结束前继续拒绝新测试及影响电源的命令。迟到结果或确定掉电仅通过RECOVERED解除超时未知状态，不补发成功结果。
