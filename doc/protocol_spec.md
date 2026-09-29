# MCU–RK UART协议

对外阅读版见[MCU与RK协议Word](mcu_rk协议.docx)；本页保留 MCU 接口及实施边界的内部说明。

基线：用户于2026-09-23提供并指定`D:/pro/tlv_proto`中的`proto.c/.h`、`proto_util.c/.h`、`crc_modbus.c`及`uart-rk3588-protocol.md`。MCU按该线格式实现，保留本工程串口/RTOS，不移植Linux演示收发程序。PC协议另见[PC TCP协议](pc_tcp_protocol_spec.md)。旧版仅在[历史归档](history/rk/protocol_spec_before_20260923.md)保留。

## 1. 帧与字节序

当前调试使用115200、8N1；V2逻辑RK口为USART1，B1为UART4或USART2，B1最终端口待真机确认。

| 字段 | 偏移 | 字节数 | 规则 |
|---|---:|---:|---|
| Header | 0 | 1 | 固定0x5A |
| Length | 1 | 2 | 大端，TLV区总长度N，4..1024 |
| HCRC | 3 | 2 | 小端，覆盖Header＋Length共3字节 |
| Seq | 5 | 8 | 大端，MCU独立递增分配，RK最终结果回显 |
| TLV区 | 13 | N | 1..16个完整单元，未知单元也计数 |
| FCRC | 13+N | 2 | 小端，仅覆盖Seq＋全部TLV，共8+N字节 |
| Footer | 15+N | 1 | 固定0xA5 |

CRC使用Modbus CRC16，初值0xFFFF，右移多项式0xA001；不含Length或HCRC的帧CRC范围不能沿用旧实现。整帧20..1040字节。

每个TLV为`Type:u16BE + ValueLength:u16BE + Value`。Value的多字节整数为**小端**；高4位表示标量类型，低12位表示tag。完整TLV长度之和（含Type/Length）必须等于N。

## 2. 命令与最终结果

| Type | Value | 说明 |
|---|---|---|
| 0x0000 | 空 | 心跳，不代表RK已就绪或测试完成 |
| 0x5000 | 命令号u16LE＋可选参数 | 任务当前仅发送1..20，无参数、每帧一个命令 |
| 0x5001 | 命令号u16LE | 结果所属命令 |
| 0x9001 | 首错地址u64LE | 0=PASS；非0为首个失败物理地址 |
| 0x3001 | die u8 | RK原始颗粒编号，不能直接认作PC槽位 |
| 0x9002 | 耗时u64LE | 毫秒 |

RK文档规定测试结束后，同一51字节响应帧包含上述四种结果TLV并回显请求Seq。MCU允许四种TLV改变顺序，但每种必须恰好一个；全部验证后按Seq＋命令号交付一次。未知扩展TLV可保留并忽略，不能用缺失字段或默认值假报PASS。不再按旧41..63结果命令逐一分发，也不将0x5002当作本次结果容器。

命令号与参考名称：

| 命令号 | 名称 | 命令号 | 名称 |
|---:|---|---:|---|
| 1 | gtest-stuck-address | 11 | gtest-checkerboard |
| 2 | gtest-rand | 12 | gtest-blockseq |
| 3 | gtest-xor | 13 | gtest-walkbits0 |
| 4 | gtest-sub | 14 | gtest-walkbits1 |
| 5 | gtest-mul | 15 | gtest-bitspread |
| 6 | gtest-div | 16 | gtest-bitflip |
| 7 | gtest-or | 17 | mats |
| 8 | gtest-and | 18 | marchx |
| 9 | gtest-seqinc | 19 | marchc- |
| 10 | gtest-solidbits | 20 | marchc+ |

参考文档将21..36的IDD列为未实现，MCU请求接口拒绝这些编号；不能仅凭编号存在宣称可执行。上述名称用于对照RK资料，不自动冻结PC的算法名称白名单。

黄金帧：命令3、Seq2，总长22字节。

```text
5A 00 06 D1 D1 00 00 00 00 00 00 00 02 50 00 00 02 03 00 25 A1 A5
```

## 3. MCU接口与生命周期

- `frame.c`负责完整帧编码/解码；`tlv_unpack`接收从0x5A开始的完整帧及实际总长度，失败清空整个输出。
- `state_machine.c`处理半包、粘包、错误恢复及50ms字节间超时；完整帧后自动开始接下一帧，业务无需在每帧后手动reset。
- `rk_link_request`复制`command_id/cookie/timeout_ms`到请求队列，返回0为排队成功，-1为参数/状态错误，-2为队列满。cookie是MCU内部业务关联，不上RK线、不代替PC Seq。
- `rk_link_receive`返回0为暂无结果、1为取得结果；结果状态区分COMPLETE、TIMEOUT、TX_FAILED、INVALID、POWER_OFF，以及仅用于解除超时未知状态的RECOVERED。
- 请求/结果队列各8槽；只有一个测试在途。RK参考文档描述忙时丢新命令，所以MCU不采用旧4 pending并发，也不按500ms自动重发测试。
- `timeout_ms=0`按本次用户要求表示无限等待；非零才启用算法时限，串口50ms帧间超时不变。结果超时不证明RK已停止，MCU发一次TIMEOUT并暂停后续发送；匹配的迟到结果/B1确定掉电通过RECOVERED解除未知，不再返回第二个成功。两事件须按序交付，不覆盖队列满时尚未交付的TIMEOUT。
- 结果队列满时保留结果，待交付后才取下一请求；UART发送失败不会自动重发。心跳发送失败等待下个周期，不补发过期心跳。
- 本V2的MCU→RK心跳由同一RK任务每2000ms（2秒）尝试发送，无独立线程；它不加入在途测试、不表示RK-ready，发送失败或错过周期不补发。每轮接收最多8块（包括断流通知），随后推进事务、心跳、OTA健康并让出CPU；未消费数据保留在UART队列，半帧保留在状态机。B1按独立工程配置，非ON时不发送心跳，刷新计时并仍上报RK任务进展。PC心跳仍由PC主动请求，MCU收到后应答，不受此间隔控制。
- PC Seq仍是每连接本端发送u32流水号，与RK Seq和cookie分离。

## 4. 实施边界

正式V2支持用户确认的外部已启动模式（power/bootMode均null，reboot=false），由pc_test_service下发真实算法并转换最终结果；操作者须先让RK进入算法服务，不从心跳推断ready。带电压/启动要求的完整B1业务仍拒绝，不能凭协议解析成功直接上电。真实路径不打开`ENABLE_RK_GATEWAY_TEST`。

用户确认每算法一个最终结果、有错依首错地址区分芯片；地址映射尚未提供，不猜physicalAddr。仍需确定整机启动就绪/安全下电/取消接口与IDD测量窗口。参考目录没有实际SPL算法源码，MCU侧构建/离线测试不能证明当前RK已安装正确算法服务。

迁移及验证明细见[更新记录](rk_protocol_migration_20260923.md)。

## 5. 查表入口与业务接入现状

查表是 MCU 的代码组织方式，不是额外的线上字段；对外 Word 保留收发格式，本节说明内部调用关系。

| 查表位置 | 表名 | 实际作用 |
|---|---|---|
| `App/protocol/pc/pc_dispatch.c` | `s_command_table` | PC CommandID → 处理函数，例如0x0103进入`handle_start_test` |
| `App/tasks/rk/rk_link.c` | `s_command_table` | RK算法编号1..20 ↔ 参考名称；统一请求合法性检查与临时联调名称映射 |
| `App/protocol/rk/state_machine.c` | `s_state_handlers` | 当前接收状态 → 单字节处理函数，替代状态`switch` |
| `App/protocol/rk/frame.c` | `s_tlv_rules` | TLV Type → Value长度规则、最终结果栏位；编码、解码及四字段聚合共用 |

三个容易混淆的编号：PC的`0x0103`是开始测试业务命令；RK的`0x5000`是命令容器Type；容器Value中的小端`03 00`才是算法编号3（`gtest-xor`）。命令表不是20个MCU算法执行函数，算法实际由RK执行。

**正式V2外部已启动模式**：PC收帧 → PC命令表 → `handle_start_test` → 严格JSON校验 → `pc_test_service`查RK算法表 → `rk_link_request` → UART → 四TLV校验及Seq/命令匹配 → 正式0x0105结果对象。受理为0006 accepted；通信异常为0006 failed，不是芯片FAIL。带电压/启动要求的完整流程仍返回notReady/otaNotReady，参数错误返回invalidArgument。

**仅V2隔离联调镜像**（`ENABLE_RK_GATEWAY_TEST=ON`，默认OFF）：PC命令表 → `rk_gateway_test.c`按名称查RK命令目录 → `rk_link_request`入队 → `rk_task`组帧并发UART → 接收状态表/字段规则表解码 → Seq与命令匹配 → `rk_link_receive`交付 → 测试适配器转换成PC `0x0105`。该路径的10秒时限和die映射仅用于模拟，未进入B1。

RK收发任务、心跳、单在途及真实算法结果交付已接入外部已启动模式；完整B1电源/RK-ready闭环仍未完成。操作、改动与本轮验证见[真实RK接入](rk_real_test_20260928.md)。
