# B1同步：RK空心跳、进度与独立FAIL包

2026-09-30按用户最终确认的RK协议同步V2共享实现：普通心跳Length=0、零TLV、16字节；0x3016为1字节0..100参考进度。RK全部上行使用RK最近收到的MCU Seq，不再使用原测试Seq或RK自身心跳计数。MCU仅检查本次命令至最新成功入UART队列Seq的窗口，允许传输滞后及两包不同Seq；按单在途、详情算法号汇总四详情与独立0x1017的0/1结论。单包/进度100不完成，重复一致幂等、内容冲突保持等待。接收兼容0x0000空Value，但本端发送仅16字节格式。

窗口可过滤携带旧Seq的旧包，但若RK把同算法旧结果重新赋予当前窗口Seq，则无法区分执行实例；本轮不新增字段，不宣称已解决执行实例级防重放。

PC JSON字段不增加；0105的testStatus依据FAIL位，不再根据地址0推导PASS。进度只在RK任务内缓存，不新增PC查询/上报命令。无在途请求忽略进度，新请求清零；进度没有算法身份，不能作为完成或安全控制依据。

同步文件为frame、state_machine、rk_task、rk_link.h、pc_test_message及pc_test_service。保留B1的UART4/USART2配置、1秒心跳、非ON不发心跳/命令、真实采样统计与默认电源安全门禁；没有引入V2模拟或板级GPIO。

B1实际C/QEMU的RK事务、PC参数/组包、PC真实路径门禁及UART/IDD回归通过；独立Debug构建与工程check通过，共享源码与V2逐文件一致。没有连接B1硬件，没有烧录或更新旧首烧/OTA包，没有提交/推送。新增协议仍待B1及真实RK验收。

完整修改及复跑范围见 `D:/pro/test/doc/rk_protocol_update_20260930.md`；当前内部线格式见 `D:/pro/test/doc/protocol_spec.md`。
