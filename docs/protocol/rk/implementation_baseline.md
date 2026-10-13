# B1–RK串口实现基线

2026-09-23用户指定RK工程师`D:/pro/tlv_proto`替换旧协议；以下说明当前C实现。更新与验证见[迁移记录](../../rk_protocol_migration_20260923.md)，不再沿用旧Word中相冲突的CRC、命令字节序和结果码。

心跳/最终结果增量及进度双TLV修正以[本次同步记录](../../rk_progress_dual_tlv_20261009.md)与下列规则为准；旧日期记录仅说明当时状态。

## 线格式

`[5A][Len:2B BE][HCRC:2B LE][Seq:8B BE][TLV...][FCRC:2B LE][A5]`

- Len只计TLV区域，允许0或4..1024字节，1..3非法；整帧最大1040字节。
- HCRC采用CRC16/MODBUS，覆盖5A和2字节Len；FCRC覆盖Seq＋TLV，不含Len/HCRC。两个CRC均小端，Seq在帧CRC保护范围内。
- TLV为`Type:2B BE / ValueLen:2B BE / Value`，线上全部TLV合计0..16个。
- 普通心跳为Len=0、零TLV的16字节帧，接收兼容0x0000空Value；0x5000的Value为`CmdID:2B LE + 参数`。任务仅发送1..20无参数命令，17=MATS；IDD21..36未实现。未知Type完整保留，交业务层判断。
- 测试进度同帧含0x5001（算法u16LE）和0x3016（进度u8，0..100），各一次，顺序不限。仅在Seq窗口及在途算法匹配后缓存；单独3016无效，进度100不代表完成。
- 最终详情同帧含0x5001（命令u16LE）、0x9001（首错地址u64LE）、0x3001（die u8）、0x9002（毫秒u64LE），每种恰好一次；再收0x1017（1字节，0=PASS、1=FAIL），可另帧或与四详情合并。地址0不判PASS，详情算法与Seq窗口通过且两部分收齐后才交付一次。
- CRC/边界通过后仍须检查TLV解包结果。任意TLV损坏都清空输出并整帧拒绝，不交付有效前缀。
- 收帧字节间门限与V2均为100ms；只用于丢弃未完成的半帧，不改变算法等待、Seq窗口或心跳周期。
- MCU命令与心跳统一分配独立RK u64 Seq，RK保存最近收到的MCU Seq并按自己的定时器发送。MCU接收窗口为本次命令至最新成功入UART队列的Seq，允许滞后及连续多帧同Seq；不要求两结果包Seq相等。该Seq不能进入PC响应头，已下发测试不自动重发。

## B1运行连接

- UART4已固定对接RK UART0_M0运行协议，`B1_RK_UART`仅允许4；USART2只收RK UART2_M0_DEBUG日志，USART1为本地可切换日志出口，不再把USART2作为协议候选。
- slot0=UART4协议、slot1=USART2日志、slot2=USART1本地控制台；目前均115200/8N1，RX NORMAL DMA、IDLE/TC事件，HT被过滤。后两路由日志模块消费，不经过TLV解析器；日志配置/运行命令见[说明](../../uart_log_console_20261010.md)。
- UART DMA缓冲在AXI SRAM，TX锁/钩子/RX启动由调度后的默认任务初始化。
- B1默认pwr_ctrl=OFF，不主动发送RK命令/心跳。上电时序未冻结前无正常上电入口。

## 请求与验收边界

- 正式PC已接入外部已启动模式：power/bootMode均null、reboot=false；还须真实电源ON、操作者确认RK算法服务就绪及现有OTA/在途门禁通过。0103受理后投递RK，最终结果转换为0105，见[真实联调路径](../../real_test_sync_20260929.md)。完整自动冷重启仍拒绝。
- 请求/结果队列各8槽，只有一个测试在途；四详情与FAIL收齐才交付，结果队列满保留副本。重复一致幂等，矛盾结果保持等待；无ACK回环、无500ms重传。
- timeout_ms=0不限制算法等待；非零启用显式时限。超时报告一次TIMEOUT并暂停后续发送，直到匹配迟到结果或B1断电。迟到结果不重复报告成功，心跳/进度不解除等待。
- TX整帧入队失败返回TX_FAILED；B1 OFF拒绝新请求，已排队请求逐条报告POWER_OFF。板级UART故障恢复与OTA四任务健康接口保留。
- IDD激励/状态保持由RK产生，B1只供电/拉偏/采样；采样窗口和正常下电安全确认还缺业务握手。
- UART ORE/FE等错误恢复已有独立C离线验证，B1电气实测与掉电前TX排空仍待验收；ON门控不保证立即撤销已排队字节。
- 不移植旧PC–TLV `dispatch.c`，尤其不允许用PG12复位RK；PG12仍只属于PHY。

真板验收须在选定串口核对黄金帧、最终结果、超时/恢复和掉电流程。PC 0105的mirrorIndex原样取RK 0x3001，0=第一颗DDR、1=第二颗，其余值不推断；physicalAddr保留为空，含义暂不定义。testStatus转换为0=FAIL、1=PASS，errorType暂填空串。B1的MATS也保持真实结果，不含V2假FAIL注入；离线测试不证明真实RK算法或B1硬件通过。
