# ADR-0001：分离PC–MCU与MCU–RK协议

- 状态：已决定（框架）
- 日期：2026-08-25；2026-08-31更新PC头语义与RK实现边界

## 背景

仓库同时存在三套互相冲突的表达：

- 旧模拟测试（如 `full_chain_stress.py`）采用“结果回相同CmdID、Params表示状态”的约定；当前RK接收代码实际只按Seq匹配pending，不要求响应CmdID与请求相同。
- 外部原始Word《STM32与RK3588_UART通信协议规范--20260708》仍列有 `0x0029-0x003D` 结果/状态命令范围（含保留项）。测试工具对齐MCU单侧实现，不等于双方已经同意废弃原结果码。
- 原始Word与仓库副本 `protocol_spec.md` 并非逐项一致，且存在Type、CmdID宽度、CRC范围/字节序及Seq加入后偏移的内部矛盾；“高4位类型+低12位Tag”也可推导出 `0x5000`，不能简单视为另一套已被否决的协议。

此外，温控已从B1职责中移除：PC通过独立接口直接管理B2 MCU，B1不控制或转发温控。B1–B2 SPI只用于保活；B2使用一组共享SPI主机总线和8个独立CS，分别选择8块B1 SPI2从机。

## 决定

1. PC–MCU采用固定18字节大端头部和UTF-8 JSON包体，不再复用现有TLV帧。
2. MCU–RK当前0x5A/0xA5 TLV、Type=0x5000、CRC和Seq规则保留为固件回归基线；与外部文档的线格式差异须双方冻结，不单侧改变或宣布结果码作废。现有pending/重传/心跳代码不代表异步结果、离线判定和故障上报已经闭环。
3. MCU负责业务语义转换，不在TCP与串口之间透传原始帧。2026-08-31确认：每条TCP连接中PC和MCU分别维护本端u32发送Seq，每发一帧加1，接收不改变本端计数，不再回显请求Seq；允许同时发送和双向同号。PC Seq仅是发送流水号，业务关联另行定义，不能与RK u64 Seq混用；RK响应沿用对应RK请求Seq的规则不变。
4. PC–MCU仍由MCU作为TCP:8888单客户端server。固定头为`Length/Version/Flag/ServiceID/CommandID/Seq/Reserved`；初版`Version=1`、`Flag=8`、`ServiceID=0`，ServiceID不参与命令分发。
5. PC协议必须限制PDU长度并正确处理TCP半包、粘包和连续多帧；Length字段为u32，MCU完整PDU上限为4096字节，命令枚举和JSON字段分批冻结。
6. B1–B2 MCU SPI仍只冻结保活电气基线，不承载温控或其他业务命令。

PC固定头流式组帧由独立`pc_state_machine`显式状态机承担；cJSON解析、CommandID校验和本地响应生成集中在`pc_dispatch`；`pc_protocol`只保留通用JSON安全入口和固定头响应组帧器。TCP任务仅负责socket生命周期、状态机编排、发送和超时。PC输入不再进入RK TLV状态机，RK原始TLV和监测文本也不再直接发给PC。专用回显镜像仍只验证传输边界，不是业务响应合同。下一阶段先定义业务请求身份，再建立与独立RK Seq的关联，不能直接复用旧队列的`pc_seq`作为新协议方案。

PC工程师提供的`Packet/TcpServer/CommandEnum`草案作为线格式输入：18字节字段顺序、完整PDU Length、大端、Version=1和Flag=8与本ADR一致。其Windows示例中的9000端口、16MiB上限及多客户端线程不直接约束MCU；本项目采用TCP:8888单客户端和4096字节完整PDU上限，发送Seq和ServiceID以8月31日确认规则为准。

**实现状态**：8月25日的PC响应回显Seq决定已被8月31日规则替代；当前固件与旧网络测试仍是回显实现，尚未迁移。变更任务见 `driver_tasks.md` 9.6；更新本ADR不代表固件已经实现新合同。

## 后续冻结项

- PC命令枚举、中文职责注释和各命令JSON字段。
- PC请求、响应和非法帧黄金样例；独立发送Seq的首值、重连初始化和回绕边界，以及无Seq回显后的业务关联方式。
- PC业务命令到RK业务命令的转换表。
- RK请求、ACK、BUSY、PASS、FAIL、异步上报、心跳和重启黄金帧。
- MCU–RK的Type、CmdID宽度/结果码、CRC范围/字节序及Seq偏移统一；多TLV响应、ACK/最终结果生命周期及错误处理另行冻结。
- B1–B2 MCU SPI的保活载荷、周期、超时判定、启动行为和错误恢复。
- PC–B2温控协议由B2工程单独定义，不进入本仓库协议合同。

## 影响

- 现有TLV全链路测试只证明既有实现/模拟器约定的回归，不再代表目标PC协议，也不能代替RK双方联调或外部规范一致性验收。
- PWR、DDR、采样、Boot等本地功能等待PC命令枚举冻结后接入新dispatch。
- PC协议和RK协议分别建立黄金帧；B1–B2只增加最小保活测试。
