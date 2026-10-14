# MCU 项目接手入口

本页是仓库文档入口。状态以 **当前源码 + 本次可复现验证** 为准，不以旧文档中的“完成”字样为准。

2026-10-14 V2已经正式网络OTA安装0.1.35，30秒健康及18次TCP检查通过，070x已在板上拒绝，整轮0106/0107可用。保留V2模拟电源和第一镜像MATS假FAIL；B1仅同步源码、未烧录。未运行RK算法，B2和B1硬件验收不变；[安装记录§11](pc_test_round_protocol_20261014.md#11-v2安装0135及网络检查2026-10-14)。下方未烧录/未提交表述仅指各自阶段。

2026-10-14随后已获准实现并同步V2/B1：0106建轮、0107失败数组汇总灯和结果废弃、010A/010B SLT本地准备及PC结果回送；070x已删除，合法旧请求回notSupported。两板构建与实际C离线回归通过，未烧录；B2状态报文及真实硬件仍待。详情见[实现及验证§10](pc_test_round_protocol_20261014.md#10-整轮slt及070x删除实现2026-10-14未烧录)。下方“未接入”仅指此前文档同步阶段，Word/飞书尚未补齐之后确认的固定SLT名称及灯映射细节。

2026-10-14最新协议已同步飞书版本621及本地Word/Markdown：0106正文为{}，加热与测试期间灯态从B2通信获取；0107使用errorchip失败芯片数组，沿用mirrorIndex整数及physicalAddr字符串，按镜像汇总结果灯。保留云端新增的电压安全范围数值，修正名称和说明；默认常温仅为显示初值，PARA仍预留。旧Word已归档，28个JSON、云端全文和20页实际分页检查通过。仅同步文档，新增业务尚未接入固件；未烧录、同步B1副本或推送。详见[本轮同步记录](pc_test_round_protocol_20261014.md#9-数组稿与云端补充内容同步2026-10-14)。

同日此前已精简全篇并同步飞书版本324：0103/0105保留FUNC，0108/0109仅预留PARA；SLT使用010A/010B，index=null，仍保留两条结果，其他无内容字段留空。070x从协议移除。对外Word/Markdown仅写通信标准，内部逻辑留在本地记录。新增业务尚未接入固件，旧070x handler未删除，本次未烧录或推送远程仓库。

2026-10-13 B1完整0103/0501/0502/0505已接入初版时序、真实驱动和本轮RK普通心跳确认，PC负责DDR安全电压范围，下电直接关轨，不等RK关机应答；硬件验收仍待B1到板。最新规则见[初版电源业务](b1_power_business_20261013.md)和[PC协议](pc_tcp_protocol_spec.md)。下方较早的“尚未接通/未同步”仅指对应记录日期。

同日Word、V2/B1 Markdown和飞书已同步执行说明，Word也补齐双颗粒0105结构。旧Word归档、JSON及20页渲染检查通过；范围与验证见[本轮同步记录](pc_power_doc_sync_20261013.md)。

2026-09-29 B1已接入真实电源ON下的外部就绪算法路径、实际采样统计及TCP异常输入恢复，见[B1同步记录](../../b1/docs/real_test_sync_20260929.md)。未烧录，首板时序/器件/RK端口待验收，自动冷重启仍拒绝；下方日期较早的“未接入”仅指当时状态。

2026-09-28 V2连接真实RK的外部启动模式及网络助手完整HEX见[操作说明](rk_real_test_20260928.md)。V2已更新0.1.17并健康确认；实际RK算法留待操作者发起。该新增JSON已写Markdown协议，Word/飞书本轮未同步，不要用旧Word判断这个新入口不支持。

日常修改App后的操作见[V2编译与安全下载](ota_daily_download.md)：VS Code按Ctrl+Shift+B走完整OTA安全SWD流程，也可只打包或通过网络升级。已部署完整B的板卡不要直接覆盖普通App ELF。

## RK协议更新（2026-09-23）

对外 UART 协议使用[新版 RK Word](mcu_rk协议.docx)，固定文件名为 `doc/mcu_rk协议.docx`。内容已按现行 TLV 协议重写并完成 8 页实际渲染检查，包含命令表、四 TLV 结果、CRC、心跳、超时与完整报文示例；不沿用旧版自动重传和旧结果编号。以后更新前按旧文件修改时间归档到 `doc/history/rk/`，同名追加序号，不覆盖旧文件。

MCU–RK已按用户指定的`D:/pro/tlv_proto`替换旧线格式/事务层并同步B1，见[现行RK协议](protocol_spec.md)和[迁移验证记录](rk_protocol_migration_20260923.md)。旧CRC范围、命令大端、4 pending/500ms重传和旧PC–TLV调度器均不再适用；下方较早的进度表及带日期旧记录不覆盖这一结论。

## B1电源控制核对（2026-09-14）

[B1电源控制核对与训练电压方案](b1_power_control_review.md)集中记录0831版原理图证据、PC6/PC7/PG11实际作用、RK806自动启动窗口、`power`训练目标语义、代码差距及实板验收清单。B1正常电源控制不预设依赖B2；训练前调压的最坏时序仍待证明。本次只保存文档，没有修改或烧录固件。下文较早的电源阶段结论按此记录修正。

## 代码规范与结构检查

见[自定义代码规范与结构检查](code_quality_review.md)：记录2026-08-31接口/任务边界整理、中文注释约定、UART互斥与TX唤醒修复，以及保留的RK队列和硬件耦合问题。正式全量构建、6种隔离镜像及离线检查通过；本轮未烧录、未推送，也未同步B1。

## PC–MCU TCP协议框架

升级接口见[PC固件升级协议](pc_ota_protocol.md)及PC协议4.29至4.31：编号仍为0901/0902/0903，MCU动作统一加`mcu_`前缀；对应`rk_*`仅保留入口并回notSupported，W25Q256已预留两块各8MiB，尚不定义RK包体或烧录。V2/B1 MCU原始分块上限3000字节、整帧≤4096，PC按`mcu_begin`返回的blockSize分块。旧固件首次迁移须显式使用[旧动作选项](ota_daily_download.md)，不自动重发写入。详见[2026-09-24修改记录](firmware_upgrade_reservation_20260924.md)；旧日期网络测试不代表本轮新动作已完成真机验证，飞书未同步。

PC工程师使用[最新版Word](pc_tcp协议.docx)：只写连接/报文、请求JSON、字段、响应和PC执行规则。[Markdown详细定义](pc_tcp_protocol_spec.md)、[内部JSON合同](pc_json_contract.md)及电源控制记录保留实现依据、开发状态和硬件验收要求，不把这些长篇说明直接复制进对外Word。8月31日规则为双方独立递增Seq、不回显，ServiceID固定0；固件迁移见任务9.6。
Word最新版固定为`doc/pc_tcp协议.docx`，不再使用“修订版”“补齐”等后缀。每次更新前，把旧文件按其最后修改时间保存为`doc/history/pc_tcp/pc_tcp协议_yyyyMMdd_HHmmss.docx`；同秒重名时增加序号，禁止覆盖已有历史文件。历史版本见[归档记录](history/pc_tcp/README.md)。若文件正被用户编辑，先等待保存/关闭，不另造一个含糊的“最新版”。
按用户要求保留连接方式、18字节头、B1命令编号和常用JSON示例；删除开发状态、草案/待定清单、HEX长示例及MCU实现附录。软件进度与验证证据仍在本页和任务清单维护，不混入对外框架。

## 当前结论（2026-08-31；各模块按所列验证日期/证据区分）

| 模块 | 代码状态 | 当前验证证据 | 结论 |
|---|---|---|---|
| TCP server + 双协议网关 | PC状态机、JSON命令表和TCP socket三层分离；ServiceID=0、MCU独立Seq，已接0x0006及0x0501-0x0703本地业务 | PC离线24/0、style 19/0、正式/网络/Mock ARM构建通过；2026-09-01 V2业务网络18/0并验证六路GPIO ODR | 传输/本地业务闭环；安全下电、Seq边界和PC到RK映射待确认 |
| Bootloader + App 偏移 | 已实现 | 本次全量编译生成 `boot.elf` / `test.elf` | 可构建，未在本次重新烧录 |
| TPS62868 / DDR 电源 API | 三轨原生10/5/5mV步进，各256档；默认标称、可显式配置训练目标 | V2/B1已共用原生换算；验证见[本轮记录](ddr_native_steps_20261012.md) | 软件基线完成，真板VOUT未闭环 |
| PAC1934T | 已实现驱动骨架 | 可编译；无本次模块/真板实测 | 未闭环 |
| AD7124 | 已实现AD7124-4驱动、事务Mock和隔离SPI波形镜像 | V2 SPI2实脚Mode 3发送事务已验收；无外接器件，MISO真实ID/电压及标定待B1 | V2发送侧闭环；B1器件侧未闭环 |
| pwr_ctrl | 已实现安全默认关闭、可配置顺序和轮询式上/下电状态机；另有隔离的V2波形镜像 | 正式镜像与`build_pwr_wave/test.elf`均构建通过；离线源码合同+3组状态迁移通过；V2断电重上电50s GPIO波形实测通过 | 软件及V2 GPIO模拟闭环；B1真板待标定 |
| LED结果显示 | 已实现PASS/FAIL/常温测试/高温测试颜色映射及500ms调度；已移植B1 PG0-PG5 | V2断电重上电50s六路功能波形通过；B1 46/46合同及ARM构建通过 | 软件/V2模拟闭环；B1真板待确认红绿/对象顺序和上电毛刺 |
| Boot 模式控制 | MCP4725 Fast Write及7种电压映射已接0x0506/0507/0508；仅OFF写寄存器、不写EEPROM，查询最近成功配置 | `boot-test` 7/7；V2网络验证无器件错误/未配置/方向路径；B1构建通过 | PC设置/查询软件闭环；0505电源循环等RK安全确认，VOUT/RK启动待B1 |
| 电压/电流监测上报 | `memory_monitor`缓存快照已接0x0601/0x0602，仅ON采集并支持部分有效 | V2网络验证OFF/validMask=0；B1监测库构建通过；无真实器件 | PC JSON软件闭环；真值和标定待B1 |
| 驱动 TLV 命令 | 仅 reboot/frame_error 为本地命令 | PWR、DDR 调压、采样、Boot 命令均未接入 | 未实现 |
| B1–B2 MCU SPI | B1引脚工程已配置SPI2从机；保活任务未实现 | 仅用于保活，不承载温控；B2共享SPI主机并用8个独立CS选择8块B1 | 硬件基线完成，保活定义待定 |
| W25Q256版本/FRU | 32MiB分区、76字节版本载荷、224字节FRU v2、双副本、CRC32和构建清单已实现；测试仪信息查询从FRU读取 | ARM构建+逐字节断电模型通过；V2板载Flash旧版实测JEDEC/QE、跨页/RMW、四字节地址、CRC回退及复位保持全部通过，v2扩展本轮待真板复测 | 存储/查询软件闭环；量产写入和真板v2复测待办 |
| B1最终引脚工程 | 同级独立目录`../b1` | 46/46引脚合同与独立构建通过；待B1真板波形验证 | 代码完成，硬件待验 |

`drvTest`现已由`ENABLE_DRIVER_SELF_TEST`编译宏隔离，默认不创建；仅开发镜像显式定义为1时访问AD7124、TPS62868、DDR power和PAC1934。

## 从零构建与离线检查

```powershell
.\tools\dev.ps1 build
.\tools\dev.ps1 rebuild
.\tools\dev.ps1 style-test
.\tools\dev.ps1 protocol-test
.\tools\dev.ps1 pc-protocol-test
.\tools\dev.ps1 boot-test
```

脚本会从 STM32Cube bundle/CMake 缓存寻找 Ninja，并以 UTF-8 模式运行 Python，避免依赖当前终端 `PATH` 和 GBK 输出编码。

硬件测试需阿波罗V2和`192.168.1.201:8888`；2026-09-23的CH340为COM5，每次先枚举确认：

```powershell
python -B -X utf8 tests\pc_business_network_test.py
# 下项仅限已备份并装入隔离联调镜像；完成后必须恢复正式镜像。
python -B -X utf8 tests\rk_gateway_network_test.py --serial COM5 --report build/rk-chain.json
```

## 文档可信度顺序

1. 当前源码、原理图/BOM、实测日志。
2. 本页和 [`driver_tasks.md`](driver_tasks.md) 的状态表。
3. [`../AGENTS.md`](../AGENTS.md) 的工程约束与操作手册。
4. [`protocol_spec.md`](protocol_spec.md)是2026-09-23按RK工程师参考包更新后的UART协议；旧提案已移入`history/rk/`，不能再作为现行实现依据。
5. [`b1_mcu_pinout.md`](b1_mcu_pinout.md) 保留B1映射依据；可执行引脚合同与最终映射在独立`../b1`工程内维护。
6. `schematic*.md` 与 AD7124 中文文档是提取/参考材料，不代表驱动已验证。

## 当前 P0 问题

- PC–MCU与MCU–RK已物理分离，正式TCP使用固定头/cJSON并停止RK TLV透传。Length为u32、MCU PDU上限4096字节；V2/B1已经按本端发送帧独立递增Seq、不回显、ServiceID固定0，并接入首批电源/采样/结果灯JSON。首值1、重连归1、回绕0目前是固件暂定且已完成V2测试，仍待PC最终确认。PC–B2温控协议属于B2工程，B1–B2只定义保活。
- `pwr_ctrl`软件状态机已建立，但启动时刻意不加载PC6/PC7量产顺序和毫秒参数，未确认配置时拒绝上电；V2同名实脚PC6/PC7/PG11的500ms测试波形已闭环，B1真板顺序/延时和最小掉电时间仍未闭环。
- RK硬复位应通过PC6对`VCC4V0_SYS`做电源循环；B1的PG12连接PHY `ETH_nRST`，当前PG12 `reboot`实现只能保留在Apollo V2，完整PC7/PC6/PG11时序待真板验证。
- 三颗双色灯对应2组LPDDR4/4X和1组eMCP；物理灯号顺序及两颗单色上电/状态灯语义待定。
- B1取消温控职责，PC通过独立接口直接管理B2 MCU；B1不转发温控命令。B1–B2 SPI仅作保活，B2为共享总线主机、8块B1为从机；Mode 0、8位MSB、硬件NSS低有效及B2初始1MHz作为联调基线，保活载荷、周期和超时判定待双方定义。
- 整机支持面板PWR键手动开机和上位机经B2 standby远程开机；当前24页B1/MB360原理图没有画出STM32 GPIO到`PWRON_L`或site PWR的控制路径，远程控制应由B2 standby/LAN Switch或未提供的B2图纸实现，接口待定义。
- TPS料号已确认为U4025=TPS628692A、U4026/U4027=TPS628681A；因子2/1/1，地址0x49/0x41/0x4F。U4026的86.8kΩ按TI标称86.6kΩ档处理。
- Apollo V2临时用SPI2开发AD7124，而最终SPI2属于B1–B2 MCU链路；必须通过板型构建配置互斥，最终AD7124改用SPI4。
- Apollo V2保留MAC `00:80:E1:00:00:00`；B1使用STM32 UID生成稳定的本地管理单播MAC，量产MAC/FRU规则冻结后再替换策略。
- Apollo V2 PHY为YT8512C，通过PCF8574 P7复位；B1 PHY为YT8512H，通过PG12 `ETH_nRST`复位，其固件在独立`../b1`工程维护。
- 驱动自检已默认关闭；开发镜像可显式定义`ENABLE_DRIVER_SELF_TEST=1`启用。
- TPS/PAC目前完成事务Mock；AD7124还完成V2 SPI2发送侧实脚波形验收。没有外接器件，三者均不能写成“真实器件或B1/B2已验证”。

暂定需求见 [`requirements_baseline.md`](requirements_baseline.md)，协议问题记录在 [`adr/0001-protocol-contract.md`](adr/0001-protocol-contract.md)，已确认的业务JSON见 [`pc_json_contract.md`](pc_json_contract.md)，规范术语见 [`../CONTEXT.md`](../CONTEXT.md)。

## 建议开发顺序

1. PC–MCU独立帧/cJSON、独立发送Seq、ServiceID=0和首批本地业务已完成V2真机验证；下一步确认Seq首值/重连/回绕，接入PC到RK业务映射并冻结无Seq回显后的请求/结果关联。与B2负责人只冻结SPI保活载荷、周期和超时，PC–B2温控协议由B2工程独立维护。
2. B1到板后在实际PG0-PG5→2N7002网络上复测结果灯波形和上电毛刺，并确认红绿顺序及三颗灯对应的测试对象；两颗单色状态灯语义仍待确认。
3. 与硬件/RK负责人冻结PC6/PC7顺序和延时后再加载配置；B1真板复测PC6/PC7/PG11、TPS62868初始化窗口及三路实际电压建立过程。
4. MCP4725驱动与B1软件移植已完成；按当前优先级继续完成TPS62868/PAC1934/AD7124正式任务接入，随后完成LPDDR上下电、拉偏与IDD业务闭环；真实器件回读留到真板。
5. W25Q256板载自检已闭环；后续仅在本地管理协议冻结后接入PC版本/FRU查询，SPD功能不实现。
6. B1/B2到板后先烧录`../b1/build_debug/b1.elf`做引脚/波形验收，再把V2验收通过的模块逐个移植到独立B1工程，完成标定、UART4/USART2、B1–B2 SPI和全链路回归。
