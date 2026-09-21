# 项目说明书（AI 接手必读）

> 本文件是给 AI 编码助手的「接手说明书」，换任何 AI 打开仓库先读这里。
> 当前进度统一维护在 [驱动任务清单](doc/driver_tasks.md#current-status)。历史记录见 [交接历史](docs/agent_handoff_history.md)，按任务需要读取。

---

## 一、项目是什么

STM32H743 **阿波罗 V2** 板，作为 **TCP server + 双协议网关**：PC–MCU 使用固定18字节头+JSON的新协议，MCU–RK 保留现有0x5A/0xA5 TLV协议；MCU负责业务转换，不做原始帧透传。

**拓扑**：
```
PC ──TCP:8888──► MCU(STM32H743) ──逻辑 RK 串口──► RK3588
                                       └─ 阿波罗 V2: USART1 / B1: UART4 或 USART2（待真机定）
PC ──独立温控接口──► B2 MCU ──► 全部加热及其他底板级控制
B2 MCU（共享SPI主机）──SCK/MOSI/MISO + CS0..7──► B1 MCU ×8（各自SPI2从机，仅保活）
```

B2 MCU由其他同事开发并由PC直接管理温控；B1不控制、代理或转发温控。B2以一组SPI主机共享SCK/MOSI/MISO，并用8个独立CS选择8块B1，SPI只用于保活。B1暂按从机、Mode 0、8位MSB、硬件NSS低有效配置，B2初始时钟暂按1MHz；保活载荷、周期、超时判定留待B2双方评审冻结。

**软件栈**：FreeRTOS 10.6.2 + LWIP 2.2.1 + ETH (YT8512C, RMII) + PC–MCU JSON协议 + MCU–RK TLV协议。B1是同级独立工程 `../b1`，不在本V2源码树内混合构建。

---

## 二、PC–MCU 与 MCU–RK 协议边界

### PC–MCU（正式TCP协议，业务接入状态见任务清单）

MCU作为TCP server等待PC连接，沿用端口8888和单客户端模型。网络字节序为大端，固定头部18字节：

```
[Length:u32] [Version:u16] [Flag:u16] [ServiceID:u16]
[CommandID:u16] [Seq:u32] [Reserved:u16] [UTF-8 JSON Body]
```

- `Length`是头部与JSON包体的总长度，字段宽度为u32；MCU完整PDU上限已确认为4096字节。
- 初版固定`Version=1`、`Flag=8`；长度、版本或Flag非法时关闭连接并清空接收状态。
- 每条连接中PC和MCU分别维护发送u32 Seq，每发一帧本端加1，不回显请求Seq；接收不改变本端发送计数，两个方向允许同号且可同时发送。Seq仅是发送流水号，业务关联另行定义；绝不能把内部RK Seq写入PC响应头。ServiceID保留2字节但固定0、不参与分发。首值、重连初始化和回绕边界仍待PC双方最终确认，暂定实现与验证状态见任务清单，测试通过不等于批准。
- 心跳固定为PC主动发送`CMD_CLIENT_HEART_BEAT(0x0002)+{}`，MCU返回`CMD_SERVER_HEART_BEAT(0x0001)+{}`，新合同使用MCU本端递增Seq；心跳不承载其他状态。
- TCP解析必须支持半包、粘包和连续多包；第一组测试JSON已确认，其余命令与字段分批冻结。
- 已固定引入cJSON v1.7.19，使用FreeRTOS堆、递归深度16；完整PDU上限4096字节。

### MCU–RK（现行协议，继续保留）

```
[0x5A] [Len:2B BE] [HCRC:2B] [Seq:8B] [TLV...] [FCRC:2B] [0xA5]
```
- CRC = Modbus CRC-16（多项式 0x8005，初值 0xFFFF）
- TLV 单元：`[Type:2B][TLV_Len:2B][Value]`
- 命令容器 `Type=0x5000`（Value = `[CmdID:2B][Params]`）；心跳 `Type=0x0000`
- 帧内 TLV 数量：**至少 1 个，最多 16 个**（`TLV_MAX_PER_FRAME`）
- 状态机 5 态：IDLE→HEADER→DATA→CRC→FOOTER；仅用于MCU–RK串口链路。

> ✅ 正式TCP不使用本TLV状态机和原始帧回传；RK组帧/串口状态机仍在使用。`dispatch.c/.h`是未接正式TCP的旧PC–TLV业务适配器，不代表整套RK协议已废弃。上述Type/CRC/Seq是当前实现基线；外部文档差异待RK双方确认，不能单侧宣布原结果命令号作废。

---

## 三、目录结构

| 目录 | 内容 |
|------|------|
| `App/protocol/pc/` | PC–MCU独立字节流状态机、JSON/命令分发、固定头响应组帧和PC业务JSON辅助层 |
| `App/protocol/rk/` | 现行RK组帧/CRC/TLV状态机；另含未接正式TCP的旧PC–TLV适配器 |
| `App/tasks/pc/` | PC侧TCP server任务；负责socket生命周期和传输编排，业务由PC命令表分发 |
| `App/tasks/rk/` | RK串口、pending/重传和RK心跳任务 |
| `App/tasks/common/` | LED、采样监控等不隶属单一通信对象的正式任务 |
| `App/tasks/test/` | Mock、自检和逻辑分析仪波形任务；不代表生产业务 |
| `BSP/` | 板级驱动：AD7124 / Flash / IIC / LED / PCF8574 / WDT |
| `HAL_Drv/` | 硬件抽象：uart_router.c（串口路由）等 |
| `Core/` `Drivers/` `Middlewares/` | 生成代码/第三方不作无关修改；允许USER CODE区及第八节列明的已有恢复项，修改后运行相关构建检查 |
| `tests/` | Python 测试：`full_chain_stress.py`（12场景）、`stability_test.py`（6项） |
| `cmake/stm32cubemx/` | CubeMX 生成，**勿手改**（自定义源放顶层 CMakeLists.txt） |

---

## 四、工具链（最重要，换 AI 必读）

### 编译
```bash
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 build
# 全量重建：powershell -ExecutionPolicy Bypass -File tools/dev.ps1 rebuild
# V2电源波形镜像：powershell -ExecutionPolicy Bypass -File tools/dev.ps1 pwr-wave-build
# V2板载W25Q256隔离自检（只使用末尾64KiB）：powershell -ExecutionPolicy Bypass -File tools/dev.ps1 flash-self-test-build
# V2 AD7124 SPI2波形镜像：powershell -ExecutionPolicy Bypass -File tools/dev.ps1 ad-wave-build
# MCP4725 Boot电压/DAC码离线合同：powershell -ExecutionPolicy Bypass -File tools/dev.ps1 boot-test
```

### 烧录（SWD）— 下列仅适用于非OTA旧布局

> 已部署完整OTA的板卡禁止用下面的`build_debug/boot.elf`覆盖B，或绕过启动日志直接覆盖App。OTA使用同级`../bootloader`完整B及`ENABLE_OTA_BOOT=ON`的槽限App；构建、备份、安装与当前板卡状态见[OTA网络验证](doc/ota_network_validation_20260916.md)。
```bash
STM32_Programmer_CLI.exe -c port=SWD -w build_debug/boot.elf   # Bootloader @ 0x08000000
STM32_Programmer_CLI.exe -c port=SWD -w build_debug/test.elf   # App @ 0x08020000
STM32_Programmer_CLI.exe -c port=SWD -rst                      # 复位运行
```
> Bootloader = 最小跳转 (boot/boot.s, 校验 App 向量表后跳转, 无升级逻辑)。
> 上述普通App编译在0x08020000。完整OTA另外限制A1/A2共同容量为896KiB，发布一个带地址修正表的`.ota`包；不依赖B2实现烧录，但升级前PC须独立协调B2维护。

### 工具链路径
```
C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/
```

### 串口 / 网络
- 串口：COM3 = CH340 → USART1（阿波罗阶段接 PC 模拟 RK3588）
- MCU IP：`192.168.1.201:8888`

### 崩溃诊断（HardFault 排查）
- 诊断地址随链接变化，读写前必须从**实际烧录的同一份ELF**用`arm-none-eabi-nm -S -n`查询`crash_log`（16×uint32）和`tcp_diag`（8×uint32）。禁止把历史地址的`-w32`命令直接套用到新镜像。`.crash_log`为NOLOAD，复位不清零。
- 反汇编：`arm-none-eabi-objdump -d --start-address=<pc-0x100> --stop-address=<pc+0x100> test.elf`
- **判断异常栈**：用 `EXC_RETURN(LR) bit2`（1=PSP线程 / 0=MSP handler），**不要**用 `__get_CONTROL()&2`（进异常时硬件已清 SPSEL，判断必错）

### App 偏移启动坑（20260817 实测发现）
- **`system_stm32h7xx.c` 的 `USER_VECT_TAB_ADDRESS` 必须解除注释**，否则 `SCB->VTOR` 设置代码被 `#if defined(...)` 编译掉 → 向量表留在 0x08000000（bootloader 空白区）→ 任何中断触发即 IACCVIOL 跑飞。CubeMX 重生成会把它注释回去，须恢复（VECT_TAB_OFFSET=0x020000 已设）。
- 诊断手法：`-coreReg PC LR MSP XPSR` 读 PC 定位跑飞点；`-r32 0xE000ED28 1`（CFSR）看故障类型；`-r32 0xE000ED08 1` 看 VTOR。

### 运行测试
```bash
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 protocol-test  # 离线帧/CRC/状态机模型
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 style-test     # 自定义层边界/事件模型+纯接口头独立编译
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 pc-protocol-test # PC固定头/cJSON离线合同
python -X utf8 tests/stability_test.py                                # 需板卡/网络/COM3
python -X utf8 tests/full_chain_stress.py                             # 需板卡/网络/COM3
```

### 工具依赖分层（换 AI 必读）

| 层 | 工具 | 换 AI 后 |
|----|------|---------|
| CLI（机器上，永不丢） | ninja / STM32_Programmer_CLI / arm-none-eabi-objdump / python / git / curl | ✅ 任何 AI 用 Bash 直接调 |
| 仓库脚本（git 内，永不丢） | tests/*.py、tools/mcp_serve.bat、.mcp.json | ✅ clone 即有 |
| MCP 服务器（绑定 Claude Code，会丢） | embedded-docs（查寄存器手册）、PlatformIO、translator | ⚠️ 降级到 CLI |

**MCP 降级方案**（换 AI 后若 MCP 不可用）：
- `embedded-docs`（查 STM32 寄存器/手册）→ 直接读 PDF 手册，或 `mineru-open-api flash-extract xxx.pdf` 转 md 读
- `PlatformIO` → **本项目未用**，编译烧录走 ninja + STM32_Programmer_CLI，无需它
- `mineru`（PDF 转 md skill）→ 底层是 npm 包，CLI 直接 `mineru-open-api flash-extract <file>` 等价

> 核心：开发主链路（编译→烧录→测试）纯 CLI 兜底，MCP 只是查手册辅助，断了可降级。

---

## 五、关键架构约束（改代码前必读）

自有代码、汇编及脚本的注释统一使用中文，说明用途、参数/返回值及关键约束；触及的自有代码遗留英文注释一并整理，第三方版权和许可保留原文。

1. **MCU 是单客户端 TCP server**：`listen(listenfd, 1)` + 单 accept 循环，一次只服务 1 个连接。测试脚本开新连接前必须 `close_tcp()`。
2. **历史RK背压基线**：`pcToRk_Queue`(8槽) + rk pending(4槽) = 最多12条排队/跟踪中的命令；旧TLV入口队列满回BUSY，pending满则暂不出队。正式PC JSON尚未接入这条业务链；结果入队失败、命令回插失败仍有丢消息风险，不称为当前完整闭环。**uart_send可能因锁竞争或TX环满失败，每处调用须有失败策略**；审查见`doc/code_quality_review.md`。
3. **当前RK响应匹配不到pending一律静默丢弃，绝不回ACK**（否则会引发ACK回环）。匹配只比较RK Seq，不要求同CmdID；首个匹配单元即释放pending。因此未匹配包也可能是尚未支持的异步/第二阶段结果，不能一概称为陈旧包；多阶段生命周期待双方评审。
4. **重传**：500ms 超时 + 指数退避 500/1000/2000ms，封顶 3 次（`RK_MAX_RETRIES`）。
5. 历史`tlv_dispatch`不再由正式`tcp_task`调用；若后续RK业务层复用其同步handler，仍禁止在handler中`vTaskDelay`阻塞。
6. **D-Cache 已关** → 无 cache 一致性顾虑，**勿再加 `SCB_InvalidateDCache_by_Addr`**（否则 UsageFault 升级 HardFault）。
7. **DMA 够不到 DTCMRAM**（DMA 在 D2 域），DMA 缓冲放 AXI SRAM (RAM, 0x24000000)。
8. **FreeRTOS 堆(128KB)手动挪到 AXI SRAM**（`heap_4.c` 的 `.heap` section + linker），不是 CubeMX 默认位置。
9. **SPI2按板型互斥**：Apollo V2临时给AD7124开发（主机Mode 3）；最终B1以SPI2从机连接B2主机，AD7124改SPI4。两个任务不得同时占用SPI2。
10. **自定义接口归属**：总线实例统一由`board_interfaces.h`声明，UART内部钩子只在`uart_hw.h`；RK任务用`rk_link_next_sequence()`分配独立序号，禁止外部直接修改计数器。`uart_start_all()`在调度前初始化TX互斥锁；分配失败拒绝发送。注释/风格及剩余耦合约束见`doc/code_quality_review.md`。
11. **RK解析错误必须上送调用方**：`tlv_sm_feed()==1`仅表示帧边界/CRC通过，随后必须检查`tlv_sm_get_frame()`返回值；`-1`时整份输出清零，任务拒绝整帧。Len为4..1024；所有线上TLV（含未知Type/心跳）合计1..16个，任何残缺TLV不得部分交付。未知Type结构有效时仍跳过，不擅自接纳其他命令容器。
12. **信息类响应的数据源**：固件版本、FRU和测试仪信息响应每次通过加载接口读取W25Q256持久化记录，禁止直接使用`firmware_version_current()`或编译期常量组帧。完整OTA由B同步运行槽版本，OTA查询每次加载并与启动记录交叉核对；普通镜像/FRU的首次初始化兼容路径另行保留，不能借OTA接入擅自删除。OTA接入范围与硬件验收状态见任务清单。

---

## 六、任务入口与硬件核对

当前进度、下一步和各层验证状态统一维护在 [驱动任务清单](doc/driver_tasks.md#current-status)。状态变化时更新该文件；本文件只维护稳定约束与入口，不继续追加里程碑。

Word文档的最新版保持固定文件名，历史版本改名归档；PC协议固定为`doc/pc_tcp协议.docx`。修改前按文件最后修改时间保存到`doc/history/pc_tcp/pc_tcp协议_yyyyMMdd_HHmmss.docx`，同秒重名加序号，不覆盖历史文件；具体规则见`doc/README.md`。不得因默认渲染工具缺失就宣称Word无法检查；可用本机WPS后台只读导出后检查实际分页，并如实记录使用的渲染方式和检查范围。

对外PC协议Word只保留PC工程师需要的请求示例、字段含义、响应关系和操作规则，说明简短；MCU内部流程、硬件时序推导、历史及开发状态留在Markdown内部文档中，不整段同步到Word。精简不能删掉PC必须遵守的安全电压、重启、index及最终结果推进规则。

按任务类型完成适用检查，软件、文档或工具任务可独立交付；缺少真板时单列硬件验收待办，不把离线/Mock/构建通过写成真机通过。具体流程见 [开发阶段与流程](doc/driver_tasks.md#execution-workflow)。旧里程碑、当时的待办和原理图摘录保存在 [交接历史](docs/agent_handoff_history.md)，其中“本轮/尚未/当前”仅指记录日期。

修改板级驱动前核对 [任务清单的引脚总表与器件章节](doc/driver_tasks.md#board-pinout)。电源控制已按 `D:\pro\MYZR-RK3588-ZUOYI_SCH_20260831-.pdf` 复核，证据、适用范围与待验收项见 [B1电源控制记录](doc/b1_power_control_review.md)；仓库0811原图仅为旧版参考，不得覆盖新结论。本次电源核对不批准IP方案或其它待定业务。

关键硬件边界：

- B1 RK链路为UART4（PA11/PA12）或USART2（PD5/PD6），各经RS0102；USART1仅本地串口。实际RK端口仍待真机确认。
- PC6控制`VCC4V0_SYS`，同时给RK806和三颗TPS供电；PC7控制独立的`VCC3V3_SYS`，不能作为暂停RK训练的开关。0831图PG11到TPS EN的串联位为NC，TPS经10kΩ上拉随4V启动，不能把PG11动作当成真实三轨开关。MCU由独立`3V3_MCU`供电，可先运行再启动RK。正常下电先取得RK安全掉电确认；故障强制关断保留。顺序/延时及RK806复位释放前的调压窗口尚待验证，旧10/10/50ms或500ms波形都不是批准值。
- `power`表示DDR初始化/训练目标，不能偷换为训练后拉偏。底层训练目标与运行拉偏缓存分离；业务通过`monitor_request_training_power_on`在OFF时选择目标并上电，默认/只选类型时仍为标称。用户接受“天然启动窗口内赶得上”作为开发预设，非硬件验收结论；寄存器可写范围不等于安全训练范围。新PC字段、MCU安全校验、配置变化强制冷重启和RK-ready闭环见任务清单，不绕过未配置时序门禁，不预设必须依赖B2。
- TPS三轨地址为0x49/0x41/0x4F，分别使用I2C1/I2C2/PH4-PH5软件开漏I2C。PAC1934同用该软件总线，7位地址0x16（总线字节0x2C/0x2D）；20260920-2134版B1三路分流电阻各0.02Ω，B1板级换算已跟随更新，V2测试配置仍为0.01Ω，不能无条件覆盖两板参数。
- B1 AD7124使用SPI4 PE11–PE14、Mode 3与内部2.5V基准；暂按AD7124-4及ID白名单0x04/0x06/0x07。图面TSSOP-24符号与LFCSP-32标注冲突，仍须硬件核对BOM/PCB。分析仪CPHA标签有歧义时查看原始上升采样沿，不据标签改为Mode 2。
- MCP4725在I2C2、地址0x60，Boot目标电压0–1.8V；不得超过1.8V，实际3V3与VOUT误差待真板校准。
- B1 PG12是PHY复位，不是RK复位。B2的`/RESET_n`接RK/RK806复位（1.8V域），`/MCU_RESET_n`接B1 NRST（3.3V域），只作异常恢复；`PWRON_L`人工按键与24V软开关不是同一网络，未见B1 GPIO驱动。
- 三颗双色结果灯使用PG0–PG5，显示2组LPDDR4/4X和1组eMCP；红绿/对象顺序及复位高阻瞬态待B1验证。红常亮FAIL、绿常亮PASS、橙闪常温测试中、红闪高温测试中，闪烁亮/灭各500ms；两颗单色灯语义仍待确认。

原理图版本变化时先核对受影响引脚、网络和电气条件并记录原图页码。`tools/sch_netlist.py`可辅助适用的矢量PDF；不适用时使用当前可用的PDF查看/渲染工具，关键网络仍须回到原图复核。

---

## 七、待定事项（决策搁置中）

| 事项 | 状态 | 何时定 |
|------|------|--------|
| MCU–RK TLV type 语义（枚举 0x5000 vs 结构化高4位+低12位） | 搁置 | RK协议评审时 |
| 0x5000→0x5002 的语义（命令请求 vs 结果响应？） | 待确认 | 同上 |
| 打包侧一帧一 TLV vs 解包侧一帧多 TLV 不对称 | 待改 | 同上（多TLV打包三段式 builder） |
| `tlv_dispatch` 多单元响应互相覆盖 | 待改 | 同上 |

---

## 八、已知坑（改代码前必读）

### CubeMX 重新生成会覆盖的手动改动
1. 自定义源/头 → 放**顶层 CMakeLists.txt**（勿放 cmake/stm32cubemx/）
2. `heap_4.c` 的 `.heap` section → 重加；长期用 `configAPPLICATION_ALLOCATED_HEAP=1`
3. `stm32h7xx_it.c` 的 `HardFault_Handler_C` → USER CODE 区恢复
4. USART TX drain → 已用 HAL `TxISR` 钩子根治，it.c 无自定义代码
5. **`system_stm32h7xx.c`**：`USER_VECT_TAB_ADDRESS` 解除注释 + `VECT_TAB_OFFSET=0x020000`（CubeMX 无对应 UI，重生成后必复位 → 手动恢复，见"App 偏移启动坑"）
6. **`STM32H743xx_FLASH.ld`**：`FLASH ORIGIN=0x08020000, LENGTH=1920K`（CubeMX 无对应 UI，重生成后复位 0x08000000/2048K → 手动恢复）
7. **`STM32H743xx_FLASH.ld`**：`.rodata`中的`KEEP(*(.firmware_version))`必须保留，否则未接入PC查询前版本元数据会被`--gc-sections`回收

> ✅ 已入 .ioc 自动保留：`SPI2.CPolarity=HIGH + ClockPhase=2EDGE`（**AD7124 模式 3** CPOL=1/CPHA=1，手册 P8 明确"operates in SPI Mode 3"，20260819 修正——此前误判为模式1）；`freertos.c` StartTestTask 自检在 USER CODE 区；boot/ 与顶层 CMakeLists 不受 CubeMX 影响。

### 测试脚本坑
- 清串口缓冲必须先 settle（等 pending+重传排空）再 `reset_input_buffer()`，否则抓到旧帧
- 全链路测试 `_drain_serial_commands()` 动态排空，别用固定 sleep

### 内存布局
- 任务栈：rk 32KB / tcp 16KB / hb 4KB；堆 128KB 实际占用约 85-100KB，余 ~30KB
- 确认堆余量用 `xPortGetFreeHeapSize()`

---

## 九、提交规范（git）

- **远程仓库**：Gitea `http://192.168.1.122:3000/LILIHONG/stm32h743`（私有），本地 remote=origin，分支 main
- 提交信息描述本次变更及相关验证；项目整体进度只在任务文档中按状态变化更新，不要求每次提交重复描述。
- 提交和推送分别按本次任务的已有授权执行；已有明确授权不重复确认。暂存前检查差异，只纳入本任务文件或改动，不用全仓`git add -A`夹带用户或其他任务的修改。按逻辑完整性组织提交，不强制每任务一次commit；提交授权不自动等于推送授权。
- 认证：用户名 `LILIHONG`，密码填 Access Token（已配置 `credential.helper store` 可免重复输入）

> 注意：组织仓库 `mcu/stm32h743` 中 LILIHONG 只有读权限，push 会拒。用 `LILIHONG/stm32h743`。
