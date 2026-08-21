# MCU 项目接手入口

本页是仓库文档入口。状态以 **当前源码 + 本次可复现验证** 为准，不以旧文档中的“完成”字样为准。

## 当前结论（2026-08-21）

| 模块 | 代码状态 | 当前验证证据 | 结论 |
|---|---|---|---|
| TCP server + TLV 网关 | 已实现 | 2026-08-14 历史提交记录 33/33；本次仅完成编译与离线协议测试 | 基线可用，需真机回归 |
| Bootloader + App 偏移 | 已实现 | 本次全量编译生成 `boot.elf` / `test.elf` | 可构建，未在本次重新烧录 |
| TPS62868 / DDR 电源 API | 已实现驱动骨架；U4027已按TPS628681A修正为因子1 | 可编译；I2C2仍为`NULL`占位；无本次模块实测 | 未闭环 |
| PAC1934T | 已实现驱动骨架 | 可编译；无本次模块/真板实测 | 未闭环 |
| AD7124 | 已实现驱动骨架 | 可编译；无本次模块/真板实测 | 未闭环 |
| pwr_ctrl | 只有 GPIO 映射 | `bsp_gpio_init()` 未接入启动流程；无 `pwr_on/off/status` | 未实现 |
| Boot 模式控制 | 无 MCP4725/boot_mode 代码 | 文档曾误写“代码完成” | 未实现 |
| 电压/电流监测上报 | `monitor_task` 仍是占位且发送被注释 | 无真实采样接入 | 未实现 |
| 驱动 TLV 命令 | 仅 reboot/frame_error 为本地命令 | PWR、DDR 调压、采样、Boot 命令均未接入 | 未实现 |
| B1–B2 MCU SPI | 原理图引出SPI2；尚无通信任务/协议 | 暂定B1主、B2从；B2有8路独立SPI分别连接8块B1 | 未实现 |
| LED结果显示 | 当前只有Apollo PB0/PB1心跳/连接灯 | 最终为3颗双色结果灯+2颗单色状态灯 | 未实现 |
| FRU | 无数据结构/读写流程 | 已确认存放于W25Q256 | 未实现 |
| B1最终引脚工程 | `b1_pincheck.elf`已建立 | 46/46引脚合同通过，双目标编译成功；待B1真板波形验证 | 代码完成，硬件待验 |

`drvTest`现已由`ENABLE_DRIVER_SELF_TEST`编译宏隔离，默认不创建；仅开发镜像显式定义为1时访问AD7124、TPS62868、DDR power和PAC1934。

## 从零构建与离线检查

```powershell
.\tools\dev.ps1 build
.\tools\dev.ps1 rebuild
.\tools\dev.ps1 protocol-test
```

脚本会从 STM32Cube bundle/CMake 缓存寻找 Ninja，并以 UTF-8 模式运行 Python，避免依赖当前终端 `PATH` 和 GBK 输出编码。

硬件测试仍需阿波罗 V2、COM3 和 `192.168.1.201:8888`：

```powershell
python -X utf8 tests\stability_test.py
python -X utf8 tests\full_chain_stress.py
```

## 文档可信度顺序

1. 当前源码、原理图/BOM、实测日志。
2. 本页和 [`driver_tasks.md`](driver_tasks.md) 的状态表。
3. [`../AGENTS.md`](../AGENTS.md) 的工程约束与操作手册。
4. [`protocol_spec.md`](protocol_spec.md) 是旧协议提案，含 `0x5002` 等未采用设计，不能直接作为现行协议。
5. [`b1_mcu_pinout.md`](b1_mcu_pinout.md) 与 `boards/b1/Core/Inc/main.h`记录最终B1映射；静态合同由`tests/verify_b1_project.py`检查。
6. `schematic*.md` 与 AD7124 中文文档是提取/参考材料，不代表驱动已验证。

## 当前 P0 问题

- PC、B1 MCU、RK、B2 MCU的正式协议合同未冻结：当前固件和本轮已对齐的测试工具使用“响应同 CmdID”，旧规范仍保留 `0x0029-0x003D` 结果区和 `Type=0x5002` 候选设计。
- pwr_ctrl 尚不存在，DDR 驱动不能形成安全的完整上电/下电闭环。
- RK硬复位应通过PC6对`VCC4V0_SYS`做电源循环；B1的PG12连接PHY `ETH_nRST`，当前PG12 `reboot`实现只能保留在Apollo V2，完整PC7/PC6/PG11时序待真板验证。
- 三颗双色灯对应2组LPDDR4/4X和1组eMCP；物理灯号顺序及两颗单色上电/状态灯语义待定。
- B1–B2 MCU SPI暂定B1主、B2从；模式、速率、轮询周期和加热/底板业务命令待双方定义。
- 整机支持面板PWR键手动开机和上位机经B2 standby远程开机；当前24页B1/MB360原理图没有画出STM32 GPIO到`PWRON_L`或site PWR的控制路径，远程控制应由B2 standby/LAN Switch或未提供的B2图纸实现，接口待定义。
- TPS料号已确认为U4025=TPS628692A、U4026/U4027=TPS628681A；因子2/1/1，地址0x49/0x41/0x4F。U4026的86.8kΩ按TI标称86.6kΩ档处理。
- Apollo V2临时用SPI2开发AD7124，而最终SPI2属于B1–B2 MCU链路；必须通过板型构建配置互斥，最终AD7124改用SPI4。
- Apollo V2保留MAC `00:80:E1:00:00:00`；B1使用STM32 UID生成稳定的本地管理单播MAC，量产MAC/FRU规则冻结后再替换策略。
- 驱动自检已默认关闭；开发镜像可显式定义`ENABLE_DRIVER_SELF_TEST=1`启用。
- 新驱动目前只有“编译通过”，不能写成“V2 或 B1/B2 已验证”。

暂定需求见 [`requirements_baseline.md`](requirements_baseline.md)，协议问题记录在 [`adr/0001-protocol-contract.md`](adr/0001-protocol-contract.md)，术语见 [`glossary.md`](glossary.md)。

## 建议开发顺序

1. 与PC、RK和B2 MCU负责人冻结共享协议：各链路黄金帧、SPI参数、ACK/BUSY/PASS/FAIL/告警/重启/加热。
2. 将三颗双色结果灯状态机落地；确认物理灯号顺序和两颗单色状态灯语义。
3. 完成`pwr_ctrl`状态机：PC6/PC7遵循RK3588/RK806要求，PG11后通过I2C配置TPS62868和LPDDR上电时序。
4. 逐个完成TPS62868→PAC1934→AD7124的Mock、波形验证和协议接入；真实器件回读留到真板。
5. 定义W25Q256内的版本信息和FRU格式；SPD功能不实现。
6. B1/B2到板后先烧录`b1_pincheck.elf`做引脚/波形验收，再把V2已验证模块接入B1目标并完成标定、UART4/USART2、B1–B2 SPI和全链路回归。
