# MCU 项目接手入口

本页是仓库文档入口。状态以 **当前源码 + 本次可复现验证** 为准，不以旧文档中的“完成”字样为准。

## 代码规范与结构检查

见[自定义代码规范与结构检查](code_quality_review.md)：记录2026-08-31接口/任务边界整理、中文注释约定、UART互斥与TX唤醒修复，以及保留的RK队列和硬件耦合问题。正式全量构建、6种隔离镜像及离线检查通过；本轮未烧录、未推送，也未同步B1。

## PC–MCU TCP协议框架

供PC工程师完善的[协议框架（Markdown）](pc_tcp_protocol_spec.md)和[Word修订版](pc_tcp_protocol_spec_协议修订.docx)。8月31日规则为双方独立递增Seq、不回显，ServiceID固定0；固件迁移见任务9.6。旧Word保留，不覆盖用户正在编辑的文件。
按用户要求保留连接方式、18字节头、B1命令编号和常用JSON示例；删除开发状态、草案/待定清单、HEX长示例及MCU实现附录。软件进度与验证证据仍在本页和任务清单维护，不混入对外框架。

## 当前结论（2026-08-31；各模块按所列验证日期/证据区分）

| 模块 | 代码状态 | 当前验证证据 | 结论 |
|---|---|---|---|
| TCP server + 双协议网关 | PC字节流状态机、JSON/查表命令分发和TCP socket任务三层分离；正式TCP不再解析或回传RK TLV，已实现心跳和0x0101/0x0102测试仪信息查询 | 8月31日PC离线合同19/0、RK模型65/0及正式/网络/Mock构建通过；历史V2真实TCP网络17/0。新增查询/分层/查表及Mock用例未在本轮烧录联网复测 | 传输/本地dispatch闭环；PC到RK业务适配和内部Seq映射待接入 |
| Bootloader + App 偏移 | 已实现 | 本次全量编译生成 `boot.elf` / `test.elf` | 可构建，未在本次重新烧录 |
| TPS62868 / DDR 电源 API | 已实现三路独立20mV步进全可写范围；初始化仍回标称 | V2离线/Mock通过；B1已绑定I2C1/I2C2/PH4-PH5软件I2C并通过ARM构建 | 软件基线完成，真板VOUT未闭环 |
| PAC1934T | 已实现驱动骨架 | 可编译；无本次模块/真板实测 | 未闭环 |
| AD7124 | 已实现AD7124-4驱动、事务Mock和隔离SPI波形镜像 | V2 SPI2实脚Mode 3发送事务已验收；无外接器件，MISO真实ID/电压及标定待B1 | V2发送侧闭环；B1器件侧未闭环 |
| pwr_ctrl | 已实现安全默认关闭、可配置顺序和轮询式上/下电状态机；另有隔离的V2波形镜像 | 正式镜像与`build_pwr_wave/test.elf`均构建通过；离线源码合同+3组状态迁移通过；V2断电重上电50s GPIO波形实测通过 | 软件及V2 GPIO模拟闭环；B1真板待标定 |
| LED结果显示 | 已实现PASS/FAIL/常温测试/高温测试颜色映射及500ms调度；已移植B1 PG0-PG5 | V2断电重上电50s六路功能波形通过；B1 46/46合同及ARM构建通过 | 软件/V2模拟闭环；B1真板待确认红绿/对象顺序和上电毛刺 |
| Boot 模式控制 | 已实现MCP4725 Fast Write、掉电模式和7种Boot电压映射；不写EEPROM | `boot-test` 7/7；V2隔离Mock实机52 PASS/0 FAIL；B1 I2C2移植构建通过 | 软件驱动闭环；reboot/PC命令和真板VOUT/RK启动待完成 |
| 电压/电流监测上报 | `memory_monitor` + `monitor_task`已生成TPS/PAC/AD结构化快照，仅电源ON时采集，支持部分有效标志 | V2隔离Mock总计74 PASS/0 FAIL（含PC传输与首组业务JSON）；B1监测库已编译接入；无真实器件 | 正式软件接入完成；PC JSON查询和真板标定待办 |
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

硬件测试仍需阿波罗 V2、COM3 和 `192.168.1.201:8888`：

```powershell
python -X utf8 tests\stability_test.py
python -X utf8 tests\full_chain_stress.py
python -X utf8 tests\pc_protocol_network_test.py
```

## 文档可信度顺序

1. 当前源码、原理图/BOM、实测日志。
2. 本页和 [`driver_tasks.md`](driver_tasks.md) 的状态表。
3. [`../AGENTS.md`](../AGENTS.md) 的工程约束与操作手册。
4. [`protocol_spec.md`](protocol_spec.md) 是旧协议提案，含 `0x5002` 等未采用设计，不能直接作为现行协议。
5. [`b1_mcu_pinout.md`](b1_mcu_pinout.md) 保留B1映射依据；可执行引脚合同与最终映射在独立`../b1`工程内维护。
6. `schematic*.md` 与 AD7124 中文文档是提取/参考材料，不代表驱动已验证。

## 当前 P0 问题

- PC–MCU传输层与MCU–RK已在`App/protocol/pc/`和`App/protocol/rk/`物理分离，正式TCP也已切到PC固定头/cJSON并停止RK TLV原始帧回传；业务dispatch、其余JSON字段和双向黄金样例仍待冻结。Length字段为u32，MCU完整PDU上限已确认为4096字节；新合同要求双方按本端发送帧独立递增Seq、不回显、ServiceID固定0，内部RK Seq仍独立；旧固件回显行为待任务9.6迁移。PC–B2温控协议属于B2工程，B1–B2只定义保活。
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

1. PC–MCU独立帧/cJSON和旧合同真实TCP传输已闭环；下一步先迁移双方独立发送Seq、不回显和ServiceID固定0规则，再冻结最小业务响应合同、命令归属及无Seq回显后的业务关联，接入PC到RK适配并分别冻结两侧黄金帧。与B2负责人只冻结SPI保活载荷、周期和超时，PC–B2温控协议由B2工程独立维护。
2. B1到板后在实际PG0-PG5→2N7002网络上复测结果灯波形和上电毛刺，并确认红绿顺序及三颗灯对应的测试对象；两颗单色状态灯语义仍待确认。
3. 与硬件/RK负责人冻结PC6/PC7顺序和延时后再加载配置；B1真板复测PC6/PC7/PG11、TPS62868初始化窗口及三路实际电压建立过程。
4. MCP4725驱动与B1软件移植已完成；按当前优先级继续完成TPS62868/PAC1934/AD7124正式任务接入，随后完成LPDDR上下电、拉偏与IDD业务闭环；真实器件回读留到真板。
5. W25Q256板载自检已闭环；后续仅在本地管理协议冻结后接入PC版本/FRU查询，SPD功能不实现。
6. B1/B2到板后先烧录`../b1/build_debug/b1.elf`做引脚/波形验收，再把V2验收通过的模块逐个移植到独立B1工程，完成标定、UART4/USART2、B1–B2 SPI和全链路回归。
