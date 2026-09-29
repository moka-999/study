# B1–RK串口实现基线

2026-09-23用户指定RK工程师`D:/pro/tlv_proto`替换旧协议；以下说明当前C实现。更新与验证见[迁移记录](../../rk_protocol_migration_20260923.md)，不再沿用旧Word中相冲突的CRC、命令字节序和结果码。

## 线格式

`[5A][Len:2B BE][HCRC:2B LE][Seq:8B BE][TLV...][FCRC:2B LE][A5]`

- Len只计TLV区域，4..1024字节；整帧最大1040字节。
- HCRC采用CRC16/MODBUS，覆盖5A和2字节Len；FCRC覆盖Seq＋TLV，不含Len/HCRC。两个CRC均小端，Seq在帧CRC保护范围内。
- TLV为`Type:2B BE / ValueLen:2B BE / Value`，线上全部TLV合计1..16个。
- 0x0000为空心跳；0x5000的Value为`CmdID:2B LE + 参数`。任务仅发送1..20无参数命令，17=MATS；IDD21..36未实现。未知Type完整保留，交业务层判断。
- 最终结果同帧含0x5001（命令u16LE）、0x9001（首错地址u64LE，0=PASS）、0x3001（die u8）、0x9002（毫秒u64LE），每种必须恰好一次，顺序不限；按Seq＋命令号匹配。
- CRC/边界通过后仍须检查TLV解包结果。任意TLV损坏都清空输出并整帧拒绝，不交付有效前缀。
- MCU命令与心跳统一分配独立RK u64 Seq，RK回复使用对应请求Seq。该Seq不能进入PC响应头；已下发测试不自动重发。

## B1运行连接

- CMake `B1_RK_UART=4`暂选UART4，设为2则选USART2；两者都经过B1自己的硬件映射。软件暂选不等于已确认RK程序用哪一路。
- 只注册slot0的一路，USART1仅作本地控制台；UART4/USART2均配置115200/8N1，RX NORMAL DMA、IDLE/TC事件，HT被过滤。
- UART DMA缓冲在AXI SRAM，TX锁/钩子/RX启动由调度后的默认任务初始化。
- B1默认pwr_ctrl=OFF，不主动发送RK命令/心跳。上电时序未冻结前无正常上电入口。

## 请求与验收边界

- 正式PC算法请求尚未投递RK，RK结果也未接入PC JSON上报。队列存在不等于业务闭环。
- 请求/结果队列各8槽，只有一个测试在途；四TLV原子聚合，结果队列满保留副本。无ACK回环、无500ms重传。
- 业务显式给出结果等待时限；超时报告一次TIMEOUT并暂停后续发送，直到匹配迟到结果或B1断电。迟到结果不重复报告成功，心跳不解除等待。
- TX整帧入队失败返回TX_FAILED；B1 OFF拒绝新请求，已排队请求逐条报告POWER_OFF。板级UART故障恢复与OTA五任务健康接口保留。
- IDD激励/状态保持由RK产生，B1只供电/拉偏/采样；采样窗口和正常下电安全确认还缺业务握手。
- UART ORE/FE等错误恢复已有独立C离线验证，B1电气实测与掉电前TX排空仍待验收；ON门控不保证立即撤销已排队字节。
- 不移植旧PC–TLV `dispatch.c`，尤其不允许用PG12复位RK；PG12仍只属于PHY。

真板验收须在选定串口核对黄金帧、最终结果、超时/恢复和掉电流程，以及die到PC颗粒编号的映射。V2的PC/RK脚本模拟不证明真实RK算法或B1硬件通过。
