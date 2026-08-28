# 项目说明书（AI 接手必读）

> 本文件是给 AI 编码助手的「接手说明书」，换任何 AI 打开仓库先读这里。
> 每次里程碑更新「六、当前进度」一节。详细调试记录见 `docs/`。

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

### PC–MCU（目标协议，正式TCP传输入口已切换、业务适配尚未接入）

MCU作为TCP server等待PC连接，沿用端口8888和单客户端模型。网络字节序为大端，固定头部18字节：

```
[Length:u32] [Version:u16] [Flag:u16] [ServiceID:u16]
[CommandID:u16] [Seq:u32] [Reserved:u16] [UTF-8 JSON Body]
```

- `Length`是头部与JSON包体的总长度，字段宽度为u32；MCU完整PDU上限已确认为4096字节。
- 初版固定`Version=1`、`Flag=8`；长度、版本或Flag非法时关闭连接并清空接收状态。
- PC与MCU–RK Seq属于两个独立命名空间；MCU响应必须原样回显PC请求Seq，绝不能把内部RK Seq写入PC响应头。
- 心跳固定为PC主动发送`CMD_CLIENT_HEART_BEAT(0x0002)+{}`，MCU返回`CMD_SERVER_HEART_BEAT(0x0001)+{}`并回显同一Seq；心跳不承载其他状态。
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
- 状态机 6 态：IDLE→HEADER→DATA→CRC→FOOTER；迁移后仅用于MCU–RK串口链路。

> ✅ 正式TCP任务已停止使用本TLV状态机和原始帧回传；本协议仅保留在MCU–RK串口任务。MCU–RK的type语义仍按现行枚举常量回归，旧文档的其他候选设计继续搁置。

---

## 三、目录结构

| 目录 | 内容 |
|------|------|
| `App/protocol/pc/` | PC–MCU独立字节流状态机、JSON/命令分发、固定头响应组帧和PC业务JSON辅助层 |
| `App/protocol/rk/` | MCU–RK 0x5A/0xA5、CRC16、TLV状态机和历史分发层 |
| `App/tasks/pc/` | PC侧TCP server任务；已使用PC固定头/cJSON，业务适配和响应待接入 |
| `App/tasks/rk/` | RK串口、pending/重传和RK心跳任务 |
| `App/tasks/common/` | LED、采样监控等不隶属单一通信对象的正式任务 |
| `App/tasks/test/` | Mock、自检和逻辑分析仪波形任务；不代表生产业务 |
| `BSP/` | 板级驱动：AD7124 / Flash / IIC / LED / PCF8574 / WDT |
| `HAL_Drv/` | 硬件抽象：uart_router.c（串口路由）等 |
| `Core/` `Drivers/` `Middlewares/` | CubeMX 生成（HAL/CMSIS/FreeRTOS/LWIP），**勿手改** |
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

### 烧录（SWD）— 两步: 先 Bootloader 后 App
```bash
STM32_Programmer_CLI.exe -c port=SWD -w build_debug/boot.elf   # Bootloader @ 0x08000000
STM32_Programmer_CLI.exe -c port=SWD -w build_debug/test.elf   # App @ 0x08020000
STM32_Programmer_CLI.exe -c port=SWD -rst                      # 复位运行
```
> Bootloader = 最小跳转 (boot/boot.s, 校验 App 向量表后跳转, 无升级逻辑)。
> App 编译在 0x08020000 (linker + VECT_TAB_OFFSET 已改)。在线升级 (W25Q256 搬移) 待 B2 联调后开发。

### 工具链路径
```
C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/
```

### 串口 / 网络
- 串口：COM3 = CH340 → USART1（阿波罗阶段接 PC 模拟 RK3588）
- MCU IP：`192.168.1.201:8888`

### 崩溃诊断（HardFault 排查）
- 诊断地址：`crash_log` = 0x20009600（16×uint32），`tcp_diag` = 0x20009640（用 `nm test.elf | grep crash_log` 验证为准）→ **实测(20260817): crash_log=0x200094E0, tcp_diag=0x20009520**（.crash_log 段 NOLOAD, 复位不清零, 排查时先 `-w32 0x20009520 0 0 0 0 0 0 0 0` 清空再复位）
- 反汇编：`arm-none-eabi-objdump -d --start-address=<pc-0x100> --stop-address=<pc+0x100> test.elf`
- **判断异常栈**：用 `EXC_RETURN(LR) bit2`（1=PSP线程 / 0=MSP handler），**不要**用 `__get_CONTROL()&2`（进异常时硬件已清 SPSEL，判断必错）

### App 偏移启动坑（20260817 实测发现）
- **`system_stm32h7xx.c` 的 `USER_VECT_TAB_ADDRESS` 必须解除注释**，否则 `SCB->VTOR` 设置代码被 `#if defined(...)` 编译掉 → 向量表留在 0x08000000（bootloader 空白区）→ 任何中断触发即 IACCVIOL 跑飞。CubeMX 重生成会把它注释回去，须恢复（VECT_TAB_OFFSET=0x020000 已设）。
- 诊断手法：`-coreReg PC LR MSP XPSR` 读 PC 定位跑飞点；`-r32 0xE000ED28 1`（CFSR）看故障类型；`-r32 0xE000ED08 1` 看 VTOR。

### 运行测试
```bash
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 protocol-test  # 离线帧/CRC/状态机模型
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

1. **MCU 是单客户端 TCP server**：`listen(listenfd, 1)` + 单 accept 循环，一次只服务 1 个连接。测试脚本开新连接前必须 `close_tcp()`。
2. **背压链闭环**：`pcToRk_Queue`(8槽) + rk pending(4槽) = 最多 12 条在途。队列满→回 BUSY(0x04)，pending 满→命令滞留队列。**uart_send 可能失败（TX环满），每处调用都要想「失败怎么办」**。
3. **RK 响应匹配不到 pending 一律静默丢弃，绝不回 ACK**（否则 RK 把 ACK 当命令再回响应 → 死循环，曾观测单 seq 重传 1207 次）。
4. **重传**：500ms 超时 + 指数退避 500/1000/2000ms，封顶 3 次（`RK_MAX_RETRIES`）。
5. 历史`tlv_dispatch`不再由正式`tcp_task`调用；若后续RK业务层复用其同步handler，仍禁止在handler中`vTaskDelay`阻塞。
6. **D-Cache 已关** → 无 cache 一致性顾虑，**勿再加 `SCB_InvalidateDCache_by_Addr`**（否则 UsageFault 升级 HardFault）。
7. **DMA 够不到 DTCMRAM**（DMA 在 D2 域），DMA 缓冲放 AXI SRAM (RAM, 0x24000000)。
8. **FreeRTOS 堆(128KB)手动挪到 AXI SRAM**（`heap_4.c` 的 `.heap` section + linker），不是 CubeMX 默认位置。
9. **SPI2按板型互斥**：Apollo V2临时给AD7124开发（主机Mode 3）；最终B1以SPI2从机连接B2主机，AD7124改SPI4。两个任务不得同时占用SPI2。

---

## 六、当前进度

**阶段：网关基线历史稳定；pwr_ctrl软件基线已建立，B1 电源硬件/采样/Boot 驱动尚未闭环。**

- ✅ 2026-08-14 历史里程碑：阿波罗 V2 全链路压测 33/33、稳定性 11/11（本轮未连接硬件复测）
- ✅ 2026-08-21：从干净构建目录全量编译成功，生成 `boot.elf` / `test.elf`；离线协议模型 65/65 PASS
- ✅ 已修复 3 个网关固件 bug（重传热循环 / 转发丢命令 / ACK回环）
- ✅ 2026-08-24：结果灯V2六路功能波形闭环；首次采集发现并修复调度前数秒假高，B1真板仍需复测复位高阻瞬态
- ⚠️ TPS62868、PAC1934、AD7124 已有事务级Mock、正式快照层和隔离自检镜像；AD7124另已完成V2 SPI2实脚Mode 3发送波形验收。手头没有外接模块，真实ID/电压/电流与标定仍待B1真板
- ✅ 2026-08-25：TPS/PAC/AD事务级Mock镜像与LPDDR离线模型建立；覆盖LPDDR4/4X标称与±偏压、反序下电、跨电源周期回标称、PAC三轨IDD换算和AD7124-4 ID白名单。ARM正式/Mock构建与离线合同通过，V2实机串口结果41 PASS/0 FAIL并已恢复正式镜像；B1真值/标定仍未闭环
- ✅ 2026-08-25：固定百分比拉偏已替换为VDD1/VDD2/VDDQ独立20mV步进，覆盖TPS寄存器可写范围（VDD1 800..3340mV，VDD2/VDDQ 400..1660mV）；高压档位可能损坏DDR，仅开放显式调压接口，上电仍强制回标称。离线穷举和Mock失败回滚已覆盖，真实VOUT待B1。
- ✅ 2026-08-25：上述TPS/PAC/DDR/pwr_ctrl/memory_monitor已移植B1；VDD1/VDD2/VDDQ分别绑定I2C1/I2C2/PH4-PH5软件开漏I2C，后者支持超时、9脉冲总线恢复、PAC多字节和Send Byte操作。B1 46/46静态合同与ARM全量构建通过；启动只执行`pwr_ctrl_init(NULL)`强制全关，不访问真实器件。
- ✅ 2026-08-25：新增独立`build_ad_wave/test.elf`，V2 SPI2实脚验证AD7124复位、STATUS/ID读取、ADC_CTRL/CH0写入及DATA读取，物理时序为Mode 3、MSB、约504kHz。ATK-Logic V1.1.2.0的CPHA标签/首位对齐存在歧义：CPHA=1整字节右移，CPHA=0会在下降切换沿偶发读旧位；须查看原始波形并以上升采样沿判断，禁止据此把固件改为Mode 2。真实MISO回读仍待B1真板
- ✅ 2026-08-25：AD7124-4驱动已同步到B1 `SPI4`（PE11-PE14），仅导出`spi4_if`，首板保守时钟390.625kHz；B1静态合同46/46与ARM构建通过（B1提交`9cebabc`），未在启动期自动访问芯片，真实ID/MISO/电压与标定待B1真板
- ✅ 2026-08-25：MCP4725 Fast Write、掉电模式及7种RK Boot电压映射已实现；离线合同7/7、V2隔离Mock实机52 PASS/0 FAIL，已恢复正式镜像。B1已绑定硬件I2C2并通过46/46合同与ARM构建（B1提交`3ed8784`）；上电不自动写DAC，真实VOUT/RK启动待B1真板
- ✅ 2026-08-25：TPS/PAC/AD已接入正式`memory_monitor`结构化快照；仅在`pwr_ctrl=ON`时初始化，下电清除就绪态，单器件失败通过`valid_mask`保留其他有效数据。纯内存Mock已于V2 MCU运行59 PASS/0 FAIL并恢复正式镜像；当前无真实器件，不代表真实ID/电压/电流或标定通过
- ✅ 2026-08-24：新增`pwr_ctrl`安全状态机（默认全关、未配置拒绝上电、顺序可配置、失败全关）；ARM全量构建、3组离线状态迁移及V2 GPIO波形通过，B1真板时序与TLV仍待完成
- ✅ 2026-08-24：新增独立`build_pwr_wave/test.elf`波形镜像；经正点原子官方原理图确认，V2直接使用与B1一致的P2-31/PC6、P2-32/PC7、P3-31/PG11；断电重上电50s逻辑分析仪实测通过，三路初始低电平、首边沿无可见毛刺、正反顺序及500ms测试间隔均正常；默认正式构建关闭且B1编译期禁止
- ✅ 2026-08-24：完成结果灯纯颜色映射（PASS绿常亮/FAIL红常亮/常温橙闪/高温红闪）和独立`build_led_wave/test.elf`波形镜像；离线合同、正式镜像及波形镜像构建通过，波形镜像已烧录V2；六路实测待接线
- ⚠️ MCP4725驱动与Boot电压映射已实现；与`pwr_ctrl` reboot联动、PC命令、真实VOUT及RK启动验证尚未完成
- ✅ `drvTest`已由`ENABLE_DRIVER_SELF_TEST`隔离，默认不创建；开发镜像显式置1才访问新驱动
- ✅ B1已拆分为同级独立工程`../b1`：46/46静态合同通过，V2验收通过的模块才逐个移植
- ✅ W25Q256已定义32MiB分区、精简76字节版本载荷、224字节FRU v2、A/B双副本、CRC32和构建清单；FRU新增`tester_name/test_board`，测试仪查询从W25读取；逐字节掉电模型通过，V2板载Flash旧版真测通过（v2待复测）
- ✅ 2026-08-25：PC传输层已实现并接入正式TCP任务：固定18字节大端头、半包/粘包/连续帧、Version/Flag/Length校验、UTF-8对象与重复键/尾随数据拒绝；cJSON v1.7.19固定接入FreeRTOS堆，深度16。正式TCP已停止解析/回传RK TLV；PC主动心跳合同已实现，专用网络镜像真板17/17且正式镜像心跳响应通过；业务dispatch和PC/RK Seq映射尚未接入
- ✅ 2026-08-25：上述通用PC帧/cJSON模块已同步到独立B1工程并通过46/46静态合同及ARM构建；B1尚未创建TCP业务任务
- ✅ 2026-08-25：第一组业务JSON结构已进入类型化辅助层：START_TEST(0x0103)解析algorithmName/algorithmIndex，TEST_RESULT_REPORT(0x0105)生成testResults数组；ServiceID、site/mirror/status业务值域和最大结果数仍待冻结
- ✅ 2026-08-27：PC测试仪信息查询已接入正式TCP任务：PC发送`CMD_QUERY_TESTER_INFO(0x0101)+{}`后，MCU返回`CMD_TESTER_INFO_RESP(0x0102)`并回显ServiceID/Seq；三个响应字段从W25Q256 FRU v2读取，空白/损坏/旧版记录首次查询时懒写入A/B副本，不在连接建立时主动推送
- ✅ 2026-08-27：PC字节流解析已从通用协议/JSON模块拆为独立`pc_state_machine.c/.h`，显式使用HEADER/BODY/FRAME_READY/ERROR状态；TCP任务只调用状态机API，未复用MCU–RK状态机
- ✅ 2026-08-27：PC JSON解析、命令方向/字段校验及本地响应组帧已从`tcp_task`拆入`pc_dispatch.c/.h`；TCP任务只保留socket生命周期、状态机编排、发送和超时处理
- ✅ 2026-08-24：结果灯业务逻辑已移植B1 PG0-PG5（B1提交`f24473f`），构建通过；红绿/对象顺序和上电毛刺待真板
- ✅ 2026-08-24：B1取消温控职责，温控改为PC直连B2；B1–B2 SPI仅保活，B1基线为从机Mode 0/硬件NSS
- 📖 接手总览、暂定需求和术语：`doc/README.md`、`doc/requirements_baseline.md`、`CONTEXT.md`

**下一步（待办）**：详见 `doc/driver_tasks.md`（唯一任务来源，每任务 4 步闭环：原理图确认→编码→阿波罗V2模拟→B1/B2真板验证）
> **两阶段策略**：阶段一 = 阿波罗 V2 开发（驱动代码 + Mock + 逻辑分析仪波形验证，当前无外接模块）；阶段二 = B1/B2 真板（到货后：引脚重映射 + 实测标定 + RK/B2 MCU 联调）
1. **双协议迁移**：PC帧/cJSON解析和正式TCP入口切换已完成；下一步以PC草案枚举为输入冻结最小业务JSON/响应合同，建立业务请求到独立RK Seq的内部映射；PC响应原样回显请求Seq，MCU–RK现有TLV/pending/重传保持不变
2. LED状态语义、颜色转换、V2六路功能波形及B1 PG0-PG5软件移植已完成：B1到板后复测实际2N7002网络的上电瞬态并确认红绿/对象顺序；两颗单色状态灯语义仍待确认
3. **pwr_ctrl硬件闭环**（软件状态机与V2 GPIO波形验证已完成；待PC6/PC7顺序/延时冻结、B1真板标定和PC–MCU命令接入）— 仍阻塞电源类功能验收
4. **TPS62868/69**（A版VSET配置：VDD1/VDD2/VDDQ地址0x49/0x41/0x4F；与pwr_on联动）
5. **PAC1934T**（I2C4, 0x2C）+ **AD7124**（SPI4 PE11-14，完成Mock/波形验证后待真板标定）
6. **B1–B2 MCU SPI保活**（B2共享总线主机+8个独立CS，8块B1均为SPI2从机；Mode 0/8位MSB/初始1MHz；保活载荷/周期/超时待冻结）
7. **Boot 模式控制**（MCP4725驱动/映射/V2 Mock与B1软件移植已完成；待reboot联动、PC命令和真板VOUT/RK启动）
8. **LED结果显示**（3颗双色灯分别显示2组LPDDR4/4X和1组eMCP；另2颗单色灯显示上电/板卡状态，语义待定）
9. **W25Q256/版本信息**（测试仪信息查询已读FRU v2；固件版本查询和FRU量产写入待实现，初版不实现OTA）
   - 信息类PC响应必须每次读取W25Q256；禁止直接使用`firmware_version_current()`或编译期常量组帧。
10. B1 板到货后：UART4/USART2 从 CH340 切到 RS0102→RK3588 真机联调（**注意不是 USART1**）
11. 高波特率（460800/921600）DMA+IDLE（等真机 RS0102）

### B1 测试单元原理图（MB360）已解析结论（2026-08，几何法从 PDF 提取）

> 分析工具：`tools/sch_netlist.py`（PDF 矢量线段重建网络表，无视觉模型可用）。
> 原理图源：`docs/MYZR-RK3588-ZUOYI_SCH_20260807-.pdf`（P1-16 RK 核心板 CB314，**P17-23 MB360 = 单个B1测试单元**，P24 DIMM314 连接器）。8槽B2底板是另一块板，不能与MB360混称。

**⚠️ 重要修正**：真机 RK3588 通信**不是 USART1**！是 **UART4 (PA11=RX/PA12=TX) + USART2 (PD5=TX/PD6=RX) 各经一路 RS0102 电平转换**（U4008: UART0_M0↔UART4；U4013: UART2_M0_DEBUG↔USART2）。USART1(PA9/PA10) 只是板上本地串口。硬件上电时 RK 的 UART0_M0 是 BMC console，联调应改用 UART4/USART2（待真机验证）。

| 外设 | 引脚 | 目标 | 备注 |
|------|------|------|------|
| AD7124-4 (U4028) | SPI4: PE11=NSS PE12=SCK PE13=MISO PE14=MOSI | AIN0 ← VCC_1V8_S3_LPDDR4X / AIN2 ← VDD2_DDR_S3 / AIN4 ← VDDQ_DDR（导线追踪已确认, 各经 100R+10K+0.1uF 滤波）; AIN1/3/5=AGND, AIN6/7=NC | 符号1～24脚匹配TSSOP-24；图面`LFCSP-32`文字冲突待硬件修正。REFIN1+ ← VCC_2V5 |
| PAC1934T (U204) | 原理图逻辑I2C4 (PH4=SCL/PH5=SDA，软件开漏GPIO；不是STM32 I2C4 AF引脚对) | 地址 **0x2C/0x2D**（ADDRSEL=5.23K→GND） | SENSE1-3: VCC_1V8_S3/VDD2_DDR_S3/VDDQ_DDR 各 0.01Ω; SENSE4 未用 |
| TPS62868/69 ×3 | U4025=I2C1, U4026=I2C2, U4027=I2C4 | VDD1 / VDD2 / VDDQ | U4025 **TPS628692A**+56.2kΩ→因子2/启动1.60V/0x49；U4026 **TPS628681A**+86.8kΩ（TI 86.6kΩ档）→因子1/0.90V/0x41；U4027 **TPS628681A**+15.4kΩ→因子1/0.50V/0x4F |
| pwr_ctrl | **PC6**→MP8759 EN (VCC4V0_SYS 4.0V), **PC7**→RT8070 EN (VCC3V3_SYS 3.3V), **PG11**→3×TPS62868 EN | PC6/PC7顺序按RK3588/RK806和最终原理图要求；PG11使能TPS后通过I2C配置LPDDR三轨 | 延时和掉电顺序待真板测量，旧10/10/50ms不是批准值 |
| 整机开机 | ATX→B2 standby/LAN Switch后启动site | 两种并列入口：面板PWR键手动开机；上位机经B2 standby远程开机 | 当前24页图没有B1 MCU GPIO到site PWR；远程控制属于B2 standby/未提供图纸，接口待定 |
| RK硬复位 | PC6→MP8759→VCC4V0_SYS→RK806 PMIC | 通过`pwr_ctrl`做安全掉电/上电；不使用PG12 | P24 `PWRON_L`仅见人工按键，是否需配合触发待真板验证 |
| PHY复位 | **PG12**→`ETH_nRST`→ETH switch→B1/B2 PHY | B1 pincheck先拉低10ms再释放 | PA0是WK-UP，旧引脚表已修正 |
| LED | PG0-6 → 2N7002 | 三颗红绿双色结果灯（2×LPDDR4/4X+1×eMCP）+ 单色LED4；另有`CPU_GPIO_PWR`单色LED5 | 红常亮FAIL、绿常亮PASS、橙闪常温测试中、红闪高温测试中；闪烁亮500ms/灭500ms；两颗单色语义待定 |
| SWD/调试 | PA13=JTMS PA14=JTCK, NRST→MCU_RESET_L, BOOT0 | | |
| ETH RMII | PA1 REF_CLK / PA2 MDIO / PA7 CRS_DV / PB11 TX_EN / PC1 MDC / PC4 PC5 RXD0/1 / PG13 PG14 TXD0/1 | 经 SN74LVC1G3157×8 可选 B1(核心板)/B2(背板) | YT8512H (P23) |
| SPI2 (PB12-15, MCU_SPI_*) | B1从机 ↔ B2主机，仅保活 | B2共享SCK/MOSI/MISO并以8个独立CS选槽；B1基线Mode 0、8位MSB、硬件NSS低有效，B2初始1MHz；保活载荷/周期/超时待定义 | |

> **职责边界**：风扇PWM由RK3588控制；温控由PC直接管理B2 MCU，全部加热和其他底板级控制由B2执行，B1不参与控制或转发；B1–B2 SPI仅保活。B1正常RK上电/重启走PC6/PC7/PG11；B2的`/RESET_n`接RK3588/RK806 `RESET_L`（1.8V域），`/MCU_RESET_n`接B1 STM32 `NRST/MCU_RESET_L`（3.3V域），仅作异常恢复。P24的“VCC_24V软开关输出”与人工按键`PWRON_L`不是同一网络。

### 原理图 20260811 版新增（docs/MYZR-RK3588-ZUOYI_SCH_20260811.pdf, 当前最新）

> ⚠️ 职责修正：P18虽画出PH8/PH9/PH10候选加热电路，但B1 MCU固件不负责加热或温控转发；PC直接管理B2 MCU完成温控。**优先用本版分析**，与20260807版差异如下：

| 新增电路 | 内容 | MCU 影响 |
|----------|------|----------|
| 加热候选电路 (P18) | **PH8/PH9/PH10** → Q19/Q21/Q23 → Q18/Q20/Q22 → 加热片×3 | 不作为B1 MCU固件需求；PC直接管理B2 MCU完成实际温控 |
| **引导模式控制 (P24)** | **U4030 MCP4725A0 DAC** (SOT23-6): VDD=3V3_MCU, A0→GND(**地址 0x60**), SCL/SDA ← R7286/R7287(0R) ← **I2C2**（与 TPS62868 U4026 同总线）; VOUT → R7288(0R) → **BOOT_SARADC_IN0** → RK3588 (p<6>) | **MCU 写 DAC 电压 → 选 RK3588 引导模式**: 0V=USB / 0.3V=SD / 0.6V=EMMC / 0.9V=FSPI M0 / 1.2V=FSPI M1 / 1.5V=FSPI M2 / 1.8V=EMMC-SD（TABLE 1, 100K 上拉域）。另有 SARADC_VIN1_KEY/RECOVERY 电阻阶梯 (LEVEL1-7) + MASKROM_Key (J36/J37) |
| 连接器 | 40x2 头子 → **FX18-60P-08SV10 (30x2=60pin)** ×2；新增 J35 (CXC01L3Z) | |
| P22 | PAC1934T 采样路径新增 R7268-7271 (4×0402, 值未提取) | |
| P21 | VDD 去耦 C4094-4104 + FB0603×4 + R7272；U4028的1～24脚符号匹配AD7124-4 TSSOP-24，但图面又写`LFCSP-32`，属于封装/符号冲突，须硬件核对BOM与PCB封装；CHS-01TB 按键 | 固件暂按AD7124-4，ID白名单0x04/0x06/0x07 |
| P14/P15/P23 | TYPEC-16P 焊座替换 P1 插头；WAFER-GH125 线束座 | |

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
- **每次 commit message 必须含两部分**：「做了什么」+「项目整体进度」
- 推送：`git add -A && git commit -m "..." && git push`
- 认证：用户名 `LILIHONG`，密码填 Access Token（已配置 `credential.helper store` 可免重复输入）

> 注意：组织仓库 `mcu/stm32h743` 中 LILIHONG 只有读权限，push 会拒。用 `LILIHONG/stm32h743`。
