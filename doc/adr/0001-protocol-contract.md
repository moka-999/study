# ADR-0001：分离PC–MCU与MCU–RK协议

- 状态：已决定（框架）
- 日期：2026-08-25

## 背景

仓库同时存在三套互相冲突的表达：

- 当前固件与 `full_chain_stress.py`：RK 响应使用与请求相同的 CmdID，Params 表示 ACK/结果。
- 审计时的旧工具和说明：结果使用 `0x0029-0x003D` CmdID 区间；本轮已将仓库测试工具对齐当前固件，旧规范仍保留历史内容和警告。
- `protocol_spec.md`：还描述了 `Type=0x5002` 以及“高 4 位类型 + 低 12 位 Tag”等未采用设计。

此外，温控已从B1职责中移除：PC通过独立接口直接管理B2 MCU，B1不控制或转发温控。B1–B2 SPI只用于保活；B2使用一组共享SPI主机总线和8个独立CS，分别选择8块B1 SPI2从机。

## 决定

1. PC–MCU采用固定18字节大端头部和UTF-8 JSON包体，不再复用现有TLV帧。
2. MCU–RK继续使用当前0x5A/0xA5 TLV、CRC、pending、重传和心跳机制。
3. MCU负责业务语义转换，不在TCP与串口之间透传原始帧；PC Seq与RK Seq相互独立，PC响应必须原样回显请求Seq。
4. PC–MCU仍由MCU作为TCP:8888单客户端server。固定头为`Length/Version/Flag/ServiceID/CommandID/Seq/Reserved`；初版`Version=1`、`Flag=8`。
5. PC协议必须限制PDU长度并正确处理TCP半包、粘包和连续多帧；Length字段为u32，MCU完整PDU上限为4096字节，命令枚举和JSON字段分批冻结。
6. B1–B2 MCU SPI仍只冻结保活电气基线，不承载温控或其他业务命令。

PC固定头流式组帧由独立`pc_state_machine`显式状态机承担；cJSON解析、CommandID校验和本地响应生成集中在`pc_dispatch`；`pc_protocol`只保留通用JSON安全入口和固定头响应组帧器。TCP任务仅负责socket生命周期、状态机编排、发送和超时。PC输入不再进入RK TLV状态机，RK原始TLV和监测文本也不再直接发给PC。专用回显镜像仍只验证传输边界，不是业务响应合同。下一阶段增加PC Seq到独立RK Seq的业务映射。

PC工程师提供的`Packet/TcpServer/CommandEnum`草案作为线格式输入：18字节字段顺序、完整PDU Length、大端、Version=1和Flag=8与本ADR一致。其Windows示例中的9000端口、16MiB上限及多客户端线程不直接约束MCU；本项目采用TCP:8888单客户端和4096字节完整PDU上限，响应必须原样回显请求Seq。

## 后续冻结项

- PC命令枚举、中文职责注释和各命令JSON字段。
- PC请求、响应和非法帧黄金样例；PC响应Seq已确定为原样回显请求Seq。
- PC业务命令到RK业务命令的转换表。
- RK请求、ACK、BUSY、PASS、FAIL、异步上报、心跳和重启黄金帧。
- MCU–RK多TLV响应规则、错误处理和CmdID/Type兼容策略。
- B1–B2 MCU SPI的保活载荷、周期、超时判定、启动行为和错误恢复。
- PC–B2温控协议由B2工程单独定义，不进入本仓库协议合同。

## 影响

- 现有TLV全链路测试继续作为MCU–RK回归证据，不再代表目标PC协议。
- PWR、DDR、采样、Boot等本地功能等待PC命令枚举冻结后接入新dispatch。
- PC协议和RK协议分别建立黄金帧；B1–B2只增加最小保活测试。
