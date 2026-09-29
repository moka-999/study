# 协议目录边界

- `pc/`：PC-MCU的18字节固定头和UTF-8 JSON。PC与MCU各自维护u32发送`Seq`，不回显。
- `rk/`：以RK工程师`tlv_proto`为准的帧/CRC/字节流解析，四TLV原子聚合结果；已同步V2实现。运行连接见`../../docs/protocol/rk/implementation_baseline.md`。

两套协议的Seq相互独立。后续业务适配层只交换已解析的业务对象，禁止原始帧透传。
