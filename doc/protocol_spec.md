# MCU–RK UART协议

对外阅读版见[MCU与RK协议Word](mcu_rk协议.docx)；本页保留 MCU 接口及实施边界的内部说明。

2026-09-30新增零TLV心跳、进度及独立FAIL包，以本页和[本轮修改记录](rk_protocol_update_20260930.md)为准；既有Word尚未同步这次增量。

2026-10-10正式最终结果改为一个3/5/7 TLV包，按失败颗粒9001+3001配对生成两项0105；进度仍为5001+3016。V2/B1代码和本地Markdown已同步，Word/飞书未同步本次变化。详见[双颗粒接入记录](rk_dual_die_result_20261010.md)。

2026-11-05核对服务器最新可读源码后，两板接受完整结果中的零地址PASS明细；心跳为MCU主动、RK收帧后应答。IDD动作仍待接入；时序与启动频率已按用户确认接入无应答发送，见[时序接入](rk_timing_frequency_20261105.md)。RK应答缺口仍见[只读核查](rk_protocol_review_20261105.md)。Word/飞书未同步本次变化。

基线：用户于2026-09-23提供并指定`D:/pro/tlv_proto`中的`proto.c/.h`、`proto_util.c/.h`、`crc_modbus.c`及`uart-rk3588-protocol.md`。MCU按该线格式实现，保留本工程串口/RTOS，不移植Linux演示收发程序。PC协议另见[PC TCP协议](pc_tcp_protocol_spec.md)。旧版仅在[历史归档](history/rk/protocol_spec_before_20260923.md)保留。

## 1. 帧与字节序

当前协议口115200、8N1；V2为USART1，B1固定UART4到RK UART0_M0，USART2只收RK调试日志，USART1为B1本地日志出口。B1线路与调试波特率仍待真板验收。

| 字段 | 偏移 | 字节数 | 规则 |
|---|---:|---:|---|
| Header | 0 | 1 | 固定0x5A |
| Length | 1 | 2 | 大端，TLV区总长度N，0或4..1024；1..3非法 |
| HCRC | 3 | 2 | 小端，覆盖Header＋Length共3字节 |
| Seq | 5 | 8 | 大端；MCU下行递增，RK所有上行填最近收到的MCU Seq |
| TLV区 | 13 | N | 0..16个完整单元，N=0时不含任何TLV |
| FCRC | 13+N | 2 | 小端，仅覆盖Seq＋全部TLV，共8+N字节 |
| Footer | 15+N | 1 | 固定0xA5 |

CRC使用Modbus CRC16，初值0xFFFF，右移多项式0xA001；不含Length或HCRC的帧CRC范围不能沿用旧实现。零TLV心跳为16字节；非空帧为20..1040字节。心跳FCRC仍覆盖8字节Seq，不能省略CRC或Seq。

每个TLV为`Type:u16BE + ValueLength:u16BE + Value`。Value的多字节整数为**小端**；高4位表示标量类型，低12位表示tag。完整TLV长度之和（含Type/Length）必须等于N。

## 2. 命令与最终结果

| Type | Value | 说明 |
|---|---|---|
| 无TLV | 无 | 普通心跳，Length=0；接收仍兼容0x0000空Value |
| 0x5000 | 命令号u16LE＋可选参数 | 正式任务发送1..20 FUNC或21..36 IDD；当前均只发编号 |
| 0x5001 | 命令号u16LE | 进度或结果所属算法 |
| 0x9001 | 地址u64LE | 0表示该颗PASS，非零表示首错地址；其后必须紧跟所属0x3001 |
| 0x3001 | die u8 | DDR标志，0=第一颗、1=第二颗；原值写入PC mirrorIndex，不作偏移 |
| 0x9002 | 耗时u64LE | 毫秒 |
| 0x3016 | uint8，1字节，0..100 | 测试进度，与0x5001同帧；不是ASCII字符串 |
| 0x1017 | bool，1字节，0/1 | 整体结论，0=两颗PASS、1=至少一颗FAIL；必须在完整结果包中 |
| 0x7018 | 寄存器地址u32LE，4字节 | tag24，编解码已准备；读写操作及正式下发待定 |
| 0x101A | bool，1字节，0/1 | tag26，1进入、0退出；独立组帧已准备，正式任务尚不附带 |

RK运行期上行（普通心跳、进度和最终结果）填最近收到的MCU Seq；MCU下行命令/心跳共用递增u64序号，接收按本次命令至最近成功入UART队列Seq过滤，不要求精确等于最新值。启动频率查询0x35独立于算法事务，按本轮缓存回复，不能套用算法事务窗口。

MCU每秒发普通心跳；当前服务器RK在收帧后，空闲回零TLV心跳、测试中回5001+3016进度，最终结果可主动发送。MCU不对RK应答再回心跳。窗口内滞后或重复Seq仍允许，不要求接收Seq单调递增。有效最终结果交付后该在途结束，重复包不重复上报；不能因进度100或心跳到达就提前完成。

测试结束发送一个3/5/7 TLV包：5001算法、1017整体FAIL位、9002耗时各一次；另有0～2组相邻9001地址+3001编号。9001=0表示该颗PASS，非零表示该颗FAIL；未列出的颗粒沿用PASS约定。3/5/7 TLV总长分别为39/56/73字节，数量表示明细组数，不能直接推导失败颗数。1017必须等于“是否存在非零失败地址”，die仅0/1且不能重复（PASS明细也参与检查）。因此整体PASS带一组或两组零地址合法，整体FAIL却只有零地址仍拒绝。

三个标量及两组明细对可换序，但每个9001后须立即跟所属3001。整包校验、算法及Seq匹配才一次交付；错误整包拒绝，不保留半份明细。零地址PASS经核验后不进入内部失败数组。旧四详情/独立1017不再跨帧拼接，未知扩展/进度不能混入最终包。

该Seq不是测试实例标识：窗口能排除携带旧Seq的迟到包，无法区分被RK重新标为当前窗口Seq的同算法旧结果。初版不新增线上字段；若后续要可靠重发或并行测试，应增加独立测试实例ID。当前仍禁止自动重发算法。

进度帧总长27字节、TLV区11字节：5001/Len=2/算法号u16LE与3016/Len=1/进度u8各一次、顺序不限。单独3016、重复、宽度/值域错误拒绝；进度与最终结果分开解码，进度100不结束测试。

在单个在途测试期间，只缓存通过Seq窗口检查且算法号匹配的进度；无测试时忽略，新测试清零。算法号不等于测试执行实例ID，仍只作参考，不用于完成、超时恢复、ready或电源操作。进度100不结束测试；不新增PC进度JSON字段。

0105每算法一个包，testResults固定两项mirrorIndex=0/1。非零失败明细对应颗粒testStatus=0；零地址明细和未列者为1。PASS地址null、FAIL保留完整非零十六进制地址，真实errorType为空。siteId/measurementData在顶层共享，testTimeMs取整体9002；V2 MATS联调仅die0为FAIL/SAF、die1真实。

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

2026-10-26按用户确认接入21..36的IDD编号下发，名称使用`idd_0`等原始大小写，完整映射见[PARA接入](para_integration_20261026.md)。PC FUNC与PARA按类别分别匹配；MCU编号接入不代表真实RK算法或标准IDD条件已经验收。动作101A和地址7018仅准备编解码，不自动添加到算法帧，也不允许混入当前最终结果包。

黄金帧：命令3、Seq2，总长22字节。

```text
5A 00 06 D1 D1 00 00 00 00 00 00 00 02 50 00 00 02 03 00 25 A1 A5
```

用户提供的普通心跳：Seq5，总长16字节，双CRC已独立核验；后续5A属于下一帧候选前缀。

```text
5A 00 00 51 D3 00 00 00 00 00 00 00 05 80 08 A5
```

### 时序设置与启动查询

运行设置每帧为5000(cmd:u16LE)＋701B(value:u32LE)，整帧30字节；支持0x25、0x26、0x28..0x34，不发送未实现的0x27。逐项发送，只报告本地入UART队列，不等待或解释RK设置回包；最后一项与后续算法也保持20ms发送间隔，无自动重发。

启动查询为单个5000(0x35:u16LE)；回复5001(0x35:u16LE)＋801C(value:int64LE)，整帧34字节。Freq缺省回复-1；不绑定算法Seq窗口，MCU回复仍分配独立递增Seq。B1上电过渡仅放行此查询；其余帧仍须ON。详见[字段、阶段和验证](rk_timing_frequency_20261105.md)。

## 3. MCU接口与生命周期

- frame.c负责完整帧编解码；tlv_unpack成功返回0..16，负数才失败。正式任务用rk_report_decode处理双TLV进度，用rk_final_result_decode得到0～2组失败明细并整体交付，不再聚合两包。
- state_machine.c处理半包、粘包和错误恢复；V2/B1字节间门限均100ms，超时清半帧，完整帧连续接收无需外部reset；不改变算法等待时限。
- `rk_link_request`复制`command_id/cookie/timeout_ms`到请求队列，返回0为排队成功，-1为参数/状态错误，-2为队列满。cookie是MCU内部业务关联，不上RK线、不代替PC Seq。
- `rk_link_receive`返回0为暂无结果、1为取得结果；结果状态区分COMPLETE、TIMEOUT、TX_FAILED、INVALID、POWER_OFF，以及仅用于解除超时未知状态的RECOVERED；时序请求另用SENT表示本地整帧入UART，不代表RK成功应答。
- 请求/结果队列各8槽；只有一个测试在途。RK参考文档描述忙时丢新命令，所以MCU不采用旧4 pending并发，也不按500ms自动重发测试。
- timeout_ms=0无限等待，未收到有效完整结果就继续busy，无自动重发。非零超时发一次TIMEOUT并暂停后续发送，匹配完整迟到结果或B1确定掉电才经RECOVERED解除未知；不补报成功，不覆盖未交付的TIMEOUT。
- 结果队列满时保留结果，待交付后才取下一请求；UART发送失败不会自动重发。心跳发送失败等待下个周期，不补发过期心跳。
- V2与B1的MCU→RK心跳均由同一RK任务每1000ms（1秒）尝试发送，无独立线程；它不加入在途测试、不表示RK-ready，发送失败或错过周期不补发。每轮接收最多8块（包括断流通知），随后推进事务、心跳、OTA健康并让出CPU；未消费数据保留在UART队列，半帧保留在状态机。B1非ON时不发送心跳，刷新计时并仍上报RK任务进展。PC心跳仍由PC主动请求，MCU收到后应答，不受此间隔控制。
- PC Seq仍是每连接本端发送u32流水号，与RK Seq和cookie分离。

## 4. 实施边界

正式V2支持用户确认的外部已启动模式（power/bootMode均null，reboot=false），由pc_test_service下发真实FUNC/PARA编号并转换最终结果；操作者须先让RK进入算法服务。B1完整电源启动已配置可调初值并以本轮普通心跳确认就绪，电气及时序待验收。真实路径不打开`ENABLE_RK_GATEWAY_TEST`。

每算法一个包含两颗DDR的最终包，共享siteId、testTimeMs和measurementData；失败明细按die匹配，physicalAddr保留为空。启动/安全下电/取消和IDD合同仍待冻结；离线回放不证明真实RK算法与现场串口通过。

迁移及验证明细见[更新记录](rk_protocol_migration_20260923.md)。

## 5. 查表入口与业务接入现状

查表是 MCU 的代码组织方式，不是额外的线上字段；对外 Word 保留收发格式，本节说明内部调用关系。

| 查表位置 | 表名 | 实际作用 |
|---|---|---|
| `App/protocol/pc/pc_dispatch.c` | `s_command_table` | PC CommandID → 处理函数，0x0103/0x0108进入`handle_start_test` |
| `App/tasks/rk/rk_link.c` | `s_command_table` | RK编号1..36 ↔ 名称及FUNC/PARA类别；统一请求合法性检查 |
| `App/protocol/rk/state_machine.c` | `s_state_handlers` | 当前接收状态 → 单字节处理函数，替代状态`switch` |
| `App/protocol/rk/frame.c` | `s_tlv_rules` | TLV Type → Value长度/范围、报告栏位；编码、解码共用 |

三个容易混淆的编号：PC的`0x0103`是开始测试业务命令；RK的`0x5000`是命令容器Type；容器Value中的小端`03 00`才是算法编号3（`gtest-xor`）。命令表不是20个MCU算法执行函数，算法实际由RK执行。

**正式V2外部已启动模式**：PC帧状态机→命令表→JSON校验→pc_test_service→请求队列→UART→RK帧状态机/完整双颗粒解码→Seq/算法匹配→结果队列→两颗0105。受理0006 accepted；通信失败0006 failed，不当作芯片FAIL；电源/启动门禁不变。

**仅V2隔离联调镜像**（ENABLE_RK_GATEWAY_TEST=ON，默认OFF）：复用同一RK任务和双颗JSON组包，固定RK_SIM站点、无测量、10秒时限；本次安装未启用，B1禁止。

RK收发任务、心跳、单在途及算法结果交付已接入；完整B1电源/就绪初版已实现，硬件验收仍待真板。PARA增量见[2026-10-26接入](para_integration_20261026.md)，早期实测见[真实RK接入](rk_real_test_20260928.md)。
