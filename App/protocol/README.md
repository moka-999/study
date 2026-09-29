# 协议目录边界

本目录按通信对象物理隔离，避免同名的“帧、命令、Seq”在维护时被误用。

- `pc/`：PC-MCU协议。MCU作为TCP server，使用18字节大端固定头和UTF-8 JSON；两端分别维护u32发送`Seq`，不回显。
- `rk/`：MCU-RK协议，以RK工程师`tlv_proto`为准。使用`0x5A/0xA5`、CRC16和TLV；RK侧`Seq`为u64，由MCU独立生成，RK最终结果回显。Value整数小端，Type/Length/Seq大端，帧CRC仅覆盖Seq＋TLV。

两套协议只在`pc/pc_test_service.c`交换已解析业务对象，禁止原始帧透传。该层拥有单在途算法和连接归属，使用cookie匹配RK结果，保留PC的algorithmIndex。PC Seq与RK Seq独立，异步响应仍由TCP任务分配MCU本端Seq。
