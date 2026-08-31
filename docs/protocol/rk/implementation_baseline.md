# B1–RK串口实现基线

这是当前C实现的说明，不是宣布外部RK规范已被替代。原始《STM32与RK3588_UART通信协议规范--20260708》中的Type、CRC、结果码及多阶段返回仍需双方统一。

## 线格式

`[5A][Len:2B BE][HCRC:2B LE][Seq:8B BE][TLV...][FCRC:2B LE][A5]`

- Len只计TLV区域，4..1024字节；整帧最大1040字节。
- HCRC采用CRC16/MODBUS，覆盖5A和2字节Len；FCRC只覆盖Len和TLV，不覆盖Seq。范围暂不改变，Seq未获CRC保护是待双端评审项。
- TLV为`Type:2B BE / ValueLen:2B BE / Value`，线上全部TLV合计1..16个。
- 0x0000为空心跳；0x5000的Value为`CmdID:2B BE + 参数`。其他Type结构有效时跳过，不自动兼容0x5002。
- CRC/边界通过后仍须检查TLV解包结果。任意TLV损坏都清空输出并整帧拒绝，不交付有效前缀。
- MCU命令与心跳独立分配RK u64 Seq；重发沿用原Seq，RK回复使用对应请求Seq。该Seq不能进入PC响应头。

## B1运行连接

- CMake `B1_RK_UART=4`暂选UART4，设为2则选USART2；两者都经过B1自己的硬件映射。软件暂选不等于已确认RK程序用哪一路。
- 只注册slot0的一路，USART1仅作本地控制台；UART4/USART2均配置115200/8N1，RX NORMAL DMA、IDLE/TC事件，HT被过滤。
- UART DMA缓冲在AXI SRAM，TX锁/钩子/RX启动由调度后的默认任务初始化。
- B1默认pwr_ctrl=OFF，不主动发送RK命令/心跳。上电时序未冻结前无正常上电入口。

## 明确尚未完成

- 正式PC算法请求尚未投递RK，RK结果也未接入PC JSON上报。队列存在不等于业务闭环。
- 当前pending只比较Seq，首个匹配单元即释放；不要求相同CmdID。因此不能说原文0x0029..0x003D结果码已废弃，也不能说ACK后最终结果/同Seq多TLV已支持。
- 未匹配响应不回ACK，避免回环；异步/迟到结果分类、满队列保留、重传失败上报和心跳离线仍待完善。
- 500/1000/2000ms退避、最多3次重发沿用V2基线；测试完成时限与受理时限尚未分离。
- IDD激励/状态保持由RK产生，B1只供电/拉偏/采样；采样窗口和正常下电安全确认还缺业务握手。
- UART ORE/FE等错误后的RX恢复、掉电前TX排空未验证；ON门控不保证立即撤销已排队字节。
- 不移植旧PC–TLV `dispatch.c`，尤其不允许用PG12复位RK；PG12仍只属于PHY。

真板验收须在选定串口上核对完整十六进制黄金帧、CRC、超时/重传、掉电和多阶段测试流程。
