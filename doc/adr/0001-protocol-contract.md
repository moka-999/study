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
3. MCU负责业务语义转换，不在TCP与串口之间透传原始帧；PC Seq与RK Seq相互独立。
4. PC–MCU仍由MCU作为TCP:8888单客户端server。固定头为`Length/Version/Flag/ServiceID/CommandID/Seq/Reserved`；初版`Version=1`、`Flag=8`。
5. PC协议必须限制PDU长度并正确处理TCP半包、粘包和连续多帧；具体上限、命令枚举和JSON字段后续冻结。
6. B1–B2 MCU SPI仍只冻结保活电气基线，不承载温控或其他业务命令。

当前代码仍让TCP和RK串口共用TLV状态机，这是待迁移的历史实现，不是新的PC–MCU合同。

## 后续冻结项

- PC命令枚举、中文职责注释和各命令JSON字段。
- PC协议最大PDU具体值及请求、响应、非法帧黄金样例。
- PC业务命令到RK业务命令的转换表。
- RK请求、ACK、BUSY、PASS、FAIL、异步上报、心跳和重启黄金帧。
- MCU–RK多TLV响应规则、错误处理和CmdID/Type兼容策略。
- B1–B2 MCU SPI的保活载荷、周期、超时判定、启动行为和错误恢复。
- PC–B2温控协议由B2工程单独定义，不进入本仓库协议合同。

## 影响

- 现有TLV全链路测试继续作为MCU–RK回归证据，不再代表目标PC协议。
- PWR、DDR、采样、Boot等本地功能等待PC命令枚举冻结后接入新dispatch。
- PC协议和RK协议分别建立黄金帧；B1–B2只增加最小保活测试。
