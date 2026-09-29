# B1 RK协议同步记录（2026-09-23）

用户指定以RK工程师`D:/pro/tlv_proto`替换旧RK协议，并同步独立B1。共用的`frame.c/.h`、`state_machine.c/.h`、`rk_link.c/.h`和`rk_task.c`与V2一致；B1保留UART4/USART2选择、OFF门禁、DMA映射和OTA任务健康上报。

## 已修改

- HCRC覆盖前3字节，FCRC覆盖Seq＋TLV；Type/Length/Seq大端，Value整数/CRC小端。
- 命令0x5000、1..20无参数，17=MATS；四个结果TLV整体解码，按Seq＋命令号匹配。
- 单个在途测试，不再500ms自动重发；结果队列满保留，超时状态未知时阻止后续发送，迟到完成只解除等待。
- 请求/结果接口改为结构化对象；B1断电逐条上报POWER_OFF，不假报完成，不借用PG0–PG6结果灯作通信指示。
- 队列创建及静态检查随接口更新。B1未引入V2的临时PC/RK测试适配器。

## 验证

- `../test/tests/verify_rk_protocol_c.py --app-root D:/pro/b1`：B1实际C在QEMU通过，包括OFF取消路径。
- `../test/tests/verify_uart_idd_c.py --app-root D:/pro/b1 --compiler <现有GNU路径>`：UART恢复、RK边界与IDD统计回归通过。
- `tools/dev.ps1 check`及普通Debug构建通过。
- 正式OTA Release 0.1.15：UART4和USART2分别完成A1/A2独立构建、地址修正逐字节一致及反向一致；镜像139452字节、1346处修正。报告在`build/ota-app-0.1.15-release-a1/`及`-a1-uart2/`。

没有B1实板测试，不把V2测试当作B1验收。正式0103仍保留notReady/otaNotReady；训练电压安全限值、RK-ready/安全下电、die映射及IDD测量窗口待双方确认。未提交、未推送。

V2实际PC↔MCU↔模拟RK联调、备份与恢复证据见`D:/pro/test/doc/rk_protocol_migration_20260923.md`。

## 同日后续：查表和注释同步

- 公共7文件同步V2：5态接收函数表、TLV字段规则表、1..20算法编号/参考名称目录；请求和发送任务共用命令目录，结果仍按Seq＋命令匹配后原子交付。
- 补齐帧宏、字节序、状态/结果字段及接口的中文注释。心跳使用具名帧长宏，B1供电OFF门禁、UART选择和健康上报不变；没有引入V2临时联调适配器。
- B1实际C/QEMU新增查表边界回归、普通Debug构建和板级check全部通过；本次未连接硬件、未烧录、未提交或推送。前述OTA双槽验证是本次整理前的记录，本轮未重跑B1双槽地址修正。
- 正式PC `0x0103`仍在参数校验后返回notReady/otaNotReady，没有放行电源或RK算法；代码入口说明见`D:/pro/test/doc/protocol_spec.md`第5节。

## 心跳任务合并（2026-09-24）

- 同步V2的RK主循环心跳，删除独立hb源/头文件、声明、任务创建与构建项；保留B1电源ON检查、OFF健康上报及UART4/USART2选择。每轮最多8个接收块，心跳不加入pending，失败不补发。
- OTA只要求默认/TCP/监测/RK四项进展，保持30秒确认及RK失活停喂狗；通用Boot健康引擎和B区未改。更新B1静态检查，防止旧hb引用或第五健康要求恢复。
- B1实际RK C/QEMU、OTA C/QEMU的6种场景、普通Debug及完整check通过；OTA Release 0.1.16分别构建UART4/USART2双槽，均140012B、1371处修正，A2逐字节及反向A1一致。0.1.16仅离线候选，没有烧录或B1实板验收。
- 未连接硬件、未提交/推送；删除文件备份在`D:/pro/test/tmp/hb_merge_20260924/before/b1/`。公共RK/OTA源与V2保持一致，正式0103仍拒绝未确认业务。
