# 项目说明书（AI 接手必读）

> 本文件是给 AI 编码助手的「接手说明书」，换任何 AI 打开仓库先读这里。
> 当前进度统一维护在 [驱动任务清单](doc/driver_tasks.md#current-status)。历史记录见 [交接历史](docs/agent_handoff_history.md)，按任务需要读取。

---

## 一、项目是什么

STM32H743 **阿波罗 V2** 板，作为 **TCP server + 双协议网关**：PC–MCU 使用固定18字节头+JSON，MCU–RK以RK工程师的`D:/pro/tlv_proto`为协议基线；MCU负责业务转换，不做原始帧透传。

**拓扑**：
```
PC ──TCP:8888──► MCU(STM32H743) ──逻辑 RK 串口──► RK3588
                                       └─ 阿波罗 V2: USART1 / B1: UART4（固定RK UART0_M0）
PC ──独立温控接口──► B2 MCU ──► 全部加热及其他底板级控制
B2 MCU（共享SPI主机）──SCK/MOSI/MISO + CS0..7──► B1 MCU ×8（各自SPI2从机，保活及待实现的IP通知）
```

B2 MCU由其他同事开发并由PC直接管理温控；B1不控制、代理或转发温控。B2以一组SPI主机共享SCK/MOSI/MISO，并用8个独立CS选择8块B1；SPI需求为保活及B2向B1通知网络配置，IP通知仅确认方向和地址规则，尚未定义报文或实现。B1暂按从机、Mode 0、8位MSB、硬件NSS低有效配置，B2初始时钟暂按1MHz；保活载荷、周期、超时及IP通知合同留待B2双方评审冻结。

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
- 初版固定`Version=1`、`Flag=8`、`ServiceID=0`。非法头前缀逐字节丢弃并有界寻找下一头；两次完整帧消费之间累计丢弃超过4096字节才关闭连接。完整帧的UTF-8/JSON错误回`0006 failed/invalidArgument`并继续；认可长度后不扫描正文内部。仍无法收齐的半帧超过500ms字节间空闲则断连，正常空闲不受此限制。详见[TCP接收恢复](doc/pc_tcp_recovery_20260929.md)。
- 每条连接中PC和MCU分别维护发送u32 Seq，每发一帧本端加1，不回显请求Seq；接收不改变本端发送计数，两个方向允许同号且可同时发送。Seq仅是发送流水号，业务关联另行定义；绝不能把内部RK Seq写入PC响应头。ServiceID保留2字节但固定0、不参与分发。首值、重连初始化和回绕边界仍待PC双方最终确认，暂定实现与验证状态见任务清单，测试通过不等于批准。
- 心跳固定为PC主动发送`CMD_CLIENT_HEART_BEAT(0x0002)+{}`，MCU返回`CMD_SERVER_HEART_BEAT(0x0001)+{}`，新合同使用MCU本端递增Seq；心跳不承载其他状态。
- TCP解析必须支持半包、粘包和连续多包；第一组测试JSON已确认，其余命令与字段分批冻结。
- 已固定引入cJSON v1.7.19，使用FreeRTOS堆、递归深度16；完整PDU上限4096字节。
- 固件升级沿用0x0901–0x0903，MCU动作统一`mcu_begin/mcu_write/mcu_finish/mcu_query/mcu_cancel`；对应五个`rk_*`仅保留并回notSupported，不访问Flash/RK。W25Q256为RK预留0x00410000和0x00C10000起始的两块8MiB，不定义RK包体。旧版首次迁移须显式旧动作选项，见`doc/ota_daily_download.md`。

### MCU–RK（2026-09-23替换后的协议）

```
[0x5A] [Len:2B BE] [HCRC:2B LE] [Seq:8B BE] [TLV...] [FCRC:2B LE] [0xA5]
```
- CRC = Modbus CRC-16（多项式0x8005，右移实现0xA001，初值0xFFFF）。HCRC覆盖前3字节；FCRC仅覆盖Seq及TLV，不含Length。
- TLV 单元：`[Type:2B BE][TLV_Len:2B BE][Value]`；Value内多字节整数为小端。
- 命令容器`0x5000`的前两字节是小端CmdID，任务仅发送无参数命令1..20；17=MATS，21..36的IDD不宣称支持。普通心跳为Length=0、零TLV的16字节完整帧；接收仍兼容0x0000空Value。
- RK所有上行帧统一填写RK最近收到的MCU Seq，不回显原测试命令Seq、不使用RK自身心跳计数；MCU下行命令/心跳仍共用递增u64流水号。
- 测试中进度帧同帧携带`0x5001`算法号u16LE和`0x3016`进度u8（0..100），各一次；先检查Seq窗口和在途算法号，单独0x3016无效。RK按定时器发送，允许窗口内Seq滞后及多帧同Seq；仅保存参考值，不作为完成/ready或自动恢复依据，不增加PC JSON字段。
- 最终结果为单个3/5/7 TLV包：5001算法u16LE、1017整体FAIL位u8、9002耗时u64LE各一次；0～2组相邻9001地址u64LE＋3001 die u8为失败明细。die仅0/1且唯一，1017=0无失败、1有失败；零地址原样保留。三个标量和失败对可換序，地址必须紧跟所属die；旧四详情/独立FAIL不再拼接完成。
- 帧内TLV数量为0..16；Length仅允许0或4..1024，1..3仍非法。新合同及验证见[增量记录](doc/rk_protocol_update_20260930.md)。
- 状态机 5 态：IDLE→HEADER→DATA→CRC→FOOTER；仅用于MCU–RK串口链路。

> 旧PC–TLV `dispatch.c/.h`已删除。RK参考工程中的POSIX/UART演示代码不直接移植，复用本工程串口/FreeRTOS并补齐边界。V2正式0103支持用户确认的外部已启动模式：power/bootMode均null、reboot=false，仅执行功能算法，不接管电源/启动；默认不设算法等待超时。B1也已接入该模式，但额外要求真实电源状态ON，测量用实际采样。B1完整上电/冷重启已装载可调初值，采用本轮普通心跳就绪与直接关电；训练安全范围交由PC确认，MCU仍校验TPS范围和步进。默认OFF，V2真实电源配置仍空；见[当前业务配置与验收](doc/b1_power_business_20261013.md)。`ENABLE_RK_GATEWAY_TEST`仅保留旧隔离模拟用途，真实RK接入不打开该开关。外部已启动模式见[真实RK接入](doc/rk_real_test_20260928.md)及[B1同步记录](../b1/docs/real_test_sync_20260929.md)。

V2可显式启用`ENABLE_V2_POWER_SIMULATION`：沿用上述正式任务及真实RK，仅电源/Boot/测量改用内存模型，PC响应不附加`simulation`字段；0601在测试中只读且不因算法在途回busy。是否模拟以构建配置与安装记录为准，不能因无标记就认作实测。默认关闭、B1禁止，与旧网关联调开关互斥，不作为真实电源/IDD验收。详见[V2电源模拟](doc/v2_power_simulation_20260928.md)。

---

## 三、目录结构

| 目录 | 内容 |
|------|------|
| `App/protocol/pc/` | PC–MCU独立字节流状态机、JSON/命令分发、固定头响应组帧和PC业务JSON辅助层 |
| `App/protocol/rk/` | RK线格式组帧/CRC/TLV状态机，详情、进度和FAIL报告解码 |
| `App/tasks/pc/` | PC侧TCP server任务；负责socket生命周期和传输编排，业务由PC命令表分发 |
| `App/tasks/rk/` | 单个RK任务统一推进串口、请求/最终结果和每1秒心跳；不自动重发测试 |
| `App/tasks/common/` | LED、采样监控等不隶属单一通信对象的正式任务 |
| `App/tasks/test/` | Mock、自检和逻辑分析仪波形任务；不代表生产业务 |
| `BSP/` | 板级驱动：AD7124 / Flash / IIC / LED / PCF8574 / WDT |
| `HAL_Drv/` | 硬件抽象：uart_router.c（串口路由）等 |
| `Core/` `Drivers/` `Middlewares/` | 生成代码/第三方不作无关修改；允许USER CODE区及第八节列明的已有恢复项，修改后运行相关构建检查 |
| `tests/` | `verify_rk_protocol_c.py`实际C离线回归；`rk_gateway_network_test.py`仅用于V2临时镜像。旧RK脚本在`legacy_rk/`以文本归档 |
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

空白板首部署使用`tools/first_flash.ps1 -Action build -Version <版本>`生成固定交付包，再用包内`flash.ps1`选择板型/指定ST-Link烧写。B1有同名独立入口，共用`../bootloader/tools/first_flash.py`；备份、全空检查、B/A1/恢复包回读通过后才由C接口提交初始日志。不提供force或自动清空旧板；新入口仍待空板实测，详见[首烧说明](../bootloader/README.md#空白板首次部署v2--b1)。

日常修改V2 App请用 `tools/dev.ps1 ota-build -Version <新版本>`，或 `ota-swd -Version <新版本> -Serial <ST-Link序列号> -ConfirmIdle YES`；网络入口为 `ota-net`。VS Code默认Ctrl+Shift+B已指向安全SWD入口；操作与验证边界见[日常下载说明](doc/ota_daily_download.md)。这些入口复用完整B安装，不能用于空片首部署或B1。

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
- 串口：2026-09-23实测COM5 = CH340 → USART1（阿波罗阶段接PC模拟RK3588）；每次先枚举确认，不硬编码旧COM3。
- 当前测试MCU IP：`192.168.1.201:8888`，继续使用本地配置。
- 正式B1 IP由B2 MCU告知，不以本地固定地址或FRU配置IP作为正式来源。已确认PC配置B2、B2经SPI通知各B1，PC随后直接连接B1；仅以产线Excel第一张`IP rule`为准：PC=`192.168.210.9`，B2末段=`10*T`，B1末段=`10*T+S`（T=1～21，S=1～8）。10号/18号整机B2分别为`.100`/`.180`，不沿用原表两处笔误。报文、时机、掩码/网关、首次发现、异常与持久化待定；当前只记录，不改变测试配置、FRU布局或新增SPI命令。详见[需求基线§1.1](doc/requirements_baseline.md)。

### 崩溃诊断（HardFault 排查）
- 诊断地址随链接变化，读写前必须从**实际烧录的同一份ELF**用`arm-none-eabi-nm -S -n`查询`crash_log`（16×uint32）和`tcp_diag`（8×uint32）。禁止把历史地址的`-w32`命令直接套用到新镜像。`.crash_log`为NOLOAD，复位不清零。
- 反汇编：`arm-none-eabi-objdump -d --start-address=<pc-0x100> --stop-address=<pc+0x100> test.elf`
- **判断异常栈**：用 `EXC_RETURN(LR) bit2`（1=PSP线程 / 0=MSP handler），**不要**用 `__get_CONTROL()&2`（进异常时硬件已清 SPSEL，判断必错）

### App 偏移启动坑（20260817 实测发现）
- **`system_stm32h7xx.c` 的 `USER_VECT_TAB_ADDRESS` 必须解除注释**，否则 `SCB->VTOR` 设置代码被 `#if defined(...)` 编译掉 → 向量表留在 0x08000000（bootloader 空白区）→ 任何中断触发即 IACCVIOL 跑飞。CubeMX 重生成会把它注释回去，须恢复（VECT_TAB_OFFSET=0x020000 已设）。
- 诊断手法：`-coreReg PC LR MSP XPSR` 读 PC 定位跑飞点；`-r32 0xE000ED28 1`（CFSR）看故障类型；`-r32 0xE000ED08 1` 看 VTOR。

### 运行测试
```bash
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 protocol-test  # 实际C帧/状态机/事务的QEMU回归
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 style-test     # 自定义层边界/事件模型+纯接口头独立编译
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 pc-protocol-test # PC固定头/cJSON离线合同
python -B -X utf8 tests/verify_rk_protocol_c.py --app-root ../b1       # 编译并运行B1实际RK源码
python -B -X utf8 tests/rk_gateway_network_test.py --serial COM5 --report build/rk-chain.json # 仅限已备份并安装临时联调镜像；结束恢复正式镜像
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
2. **RK背压**：`rkRequestQueue`与`rkResponseQueue`各8槽，但只允许一个已发出的测试在途。结果队列满时保留待交付副本，不取下一请求。`uart_send`整帧入队失败返回TX_FAILED，不自动回插或重发。
3. **RK结果匹配**：RK填最近收到的MCU Seq，仅按本次命令至最新成功入UART队列Seq窗口过滤，允许滞后/重复。完整3/5/7 TLV包原子校验、算法匹配才交付一次；残缺/矛盾/重复die拒绝并保持等待。完成后重复包忽略，无自动ACK/重发；同算法旧结果被重标当前窗口Seq无法区分，不宣称执行实例级防重放。
4. **超时**：timeout_ms=0表示不限制算法等待（用户确认十几分钟算法不设时限）；非零仍为显式最终结果时限。超时只报一次TIMEOUT，RK状态未知时暂停后续命令。匹配迟到结果/B1确定掉电通过内部RECOVERED解除未知，不再报成功；恢复通知不得覆盖尚未交付的TIMEOUT。无自动重试，心跳不代表算法完成或超时恢复；B1启动编排按新约定使用本轮普通心跳确认初始化。
5. **正式业务门禁**：外部已启动模式仅下发功能算法；B1完整电源/Boot路径采用可调初值，PC确认训练安全范围，MCU校验TPS编码并等待本轮有效普通心跳，保留notReady/otaNotReady、互斥及故障处理。0105每算法一包、两颗mirrorIndex=0/1，siteId/measurementData顶层共享；缺失编号/测量如实留空。断线不取消算法，新测试/电源改动/升级写在途busy，handler不阻塞等结果。V2显式MATS假FAIL只改第一颗PC视图，第二颗真实；B1禁止。
6. **D-Cache 已关** → 无 cache 一致性顾虑，**勿再加 `SCB_InvalidateDCache_by_Addr`**（否则 UsageFault 升级 HardFault）。
7. **DMA 够不到 DTCMRAM**（DMA 在 D2 域），DMA 缓冲放 AXI SRAM (RAM, 0x24000000)。
8. **FreeRTOS 堆(128KB)手动挪到 AXI SRAM**（`heap_4.c` 的 `.heap` section + linker），不是 CubeMX 默认位置。
9. **SPI2按板型互斥**：Apollo V2临时给AD7124开发（主机Mode 3）；最终B1以SPI2从机连接B2主机，AD7124改SPI4。两个任务不得同时占用SPI2。
10. **自定义接口归属**：总线实例统一由`board_interfaces.h`声明，UART内部钩子只在`uart_hw.h`；RK任务用`rk_link_next_sequence()`分配独立序号，禁止外部直接修改计数器。`uart_start_all()`在调度前初始化TX互斥锁；分配失败拒绝发送。注释/风格及剩余耦合约束见`doc/code_quality_review.md`。
11. **RK解析错误必须上送调用方**：tlv_sm_feed完成后还须检查tlv_sm_get_frame；负数失败、0为零TLV心跳。Length为0或4..1024，最多16 TLV；底层保留未知TLV，最终业务只接受3/5/7合同，不能部分交付。tlv_unpack输入从5A起的完整帧和真实长度。V2/B1字节间门限均100ms，连续完整帧无需外部reset。
12. **信息类响应的数据源**：固件版本、FRU和测试仪信息响应每次通过加载接口读取W25Q256持久化记录，禁止直接使用`firmware_version_current()`或编译期常量组帧。完整OTA由B同步运行槽版本，OTA查询每次加载并与启动记录交叉核对；普通镜像/FRU的首次初始化兼容路径另行保留，不能借OTA接入擅自删除。OTA接入范围与硬件验收状态见任务清单。

---

## 六、任务入口与硬件核对

当前进度、下一步和各层验证状态统一维护在 [驱动任务清单](doc/driver_tasks.md#current-status)。状态变化时更新该文件；本文件只维护稳定约束与入口，不继续追加里程碑。

Word文档的最新版保持固定文件名，历史版本改名归档；PC协议固定为`doc/pc_tcp协议.docx`。修改前按文件最后修改时间保存到`doc/history/pc_tcp/pc_tcp协议_yyyyMMdd_HHmmss.docx`，同秒重名加序号，不覆盖历史文件；具体规则见`doc/README.md`。不得因默认渲染工具缺失就宣称Word无法检查；可用本机WPS后台只读导出后检查实际分页，并如实记录使用的渲染方式和检查范围。

对外PC协议Word只保留PC工程师需要的请求示例、字段含义、响应关系和操作规则，说明简短；MCU内部流程、硬件时序推导、历史及开发状态留在Markdown内部文档中，不整段同步到Word。精简不能删掉PC必须遵守的安全电压、重启、index及最终结果推进规则。

按任务类型完成适用检查，软件、文档或工具任务可独立交付；缺少真板时单列硬件验收待办，不把离线/Mock/构建通过写成真机通过。具体流程见 [开发阶段与流程](doc/driver_tasks.md#execution-workflow)。旧里程碑、当时的待办和原理图摘录保存在 [交接历史](docs/agent_handoff_history.md)，其中“本轮/尚未/当前”仅指记录日期。

修改板级驱动前核对 [任务清单的引脚总表与器件章节](doc/driver_tasks.md#board-pinout)。电源控制已按 `D:\pro\MYZR-RK3588-ZUOYI_SCH_20260831-.pdf` 复核，证据、适用范围与待验收项见 [B1电源控制记录](doc/b1_power_control_review.md)；仓库0811原图仅为旧版参考，不得覆盖新结论。本次电源核对不批准IP方案或其它待定业务。

关键硬件边界：

- B1 UART4（PA11/PA12）固定经RS0102对接RK UART0_M0跑协议；USART2（PD5/PD6）经RS0102只接收RK UART2_M0_DEBUG日志，USART1（PA9/PA10）输出所选MCU/RK日志。编译指定默认、运行时本地`log mcu`/`log rk`加换行切换，不写Flash、不转发输入给RK；当前115200/8N1仍待真板核对。V2的USART1仍是协议口，不能照搬B1日志分配。
- PC6控制`VCC4V0_SYS`，同时给RK806和三颗TPS供电；PC7控制独立的`VCC3V3_SYS`，不能作为暂停RK训练的开关。0831图PG11到TPS EN的串联位为NC，TPS经10kΩ上拉随4V启动，不能把PG11动作当成真实三轨开关。MCU由独立`3V3_MCU`供电，可先运行再启动RK。按用户确认初版直接关电，不等待RK关机应答，故障强制关断保留；这不保证RK写盘已结束。顺序/延时及RK806复位释放前的调压窗口仍须真板验证，旧波形不是批准值。
- B1初值集中在`../b1/Core/Inc/board_power_config.h`：PC7先开→10ms→PC6开→5ms后写TPS；反序下电间隔10ms；OFF停留500ms、Boot稳定10ms、切换上限2s、RK启动心跳上限60s。main绑定心跳适配器但保持OFF，V2真实电源配置未启用。初值不是硬件验收；PC负责DDR安全范围，MCU只校验TPS物理范围/步进；直接关电不等待RK写盘完成，见`doc/b1_power_business_20261013.md`。
- `power`表示DDR初始化/训练目标，不能偷换为训练后拉偏。底层训练目标与运行拉偏缓存分离；业务通过`monitor_request_training_power_on`在OFF时选择目标并上电，默认/只选类型时仍为标称。用户接受“天然启动窗口内赶得上”作为开发预设，非硬件验收结论；寄存器可写范围不等于安全训练范围。新PC字段、MCU安全校验、配置变化强制冷重启和RK-ready闭环见任务清单，不绕过缺失的配置门禁，不预设必须依赖B2。
- TPS三轨地址为0x49/0x41/0x4F，分别使用I2C1/I2C2/PH4-PH5软件开漏I2C。PAC1934同用该软件总线，7位地址0x16（总线字节0x2C/0x2D）；20260920-2134版B1三路分流电阻各0.02Ω，B1板级换算已跟随更新，V2测试配置仍为0.01Ω，不能无条件覆盖两板参数。
- 0103.power/0509使用`vdd1/vdd2/vddq`十进制字符串直接表示绝对mV，不再接收旧steps字段。共享解析器严格校验`valMv=nominalMv+stepMv*n`及TPS范围，n为整数；VDD1步进10mV、VDD2/VDDQ步进5mV，不取整。内部仍复用有符号原生档位及现有驱动；可写不等于DDR安全。范围、兼容与验证见`doc/pc_voltage_mv_20261012.md`。
- B1 AD7124使用SPI4 PE11–PE14、Mode 3与内部2.5V基准；暂按AD7124-4及ID白名单0x04/0x06/0x07。图面TSSOP-24符号与LFCSP-32标注冲突，仍须硬件核对BOM/PCB。分析仪CPHA标签有歧义时查看原始上升采样沿，不据标签改为Mode 2。
- MCP4725在I2C2、地址0x60，Boot目标电压0–1.8V；不得超过1.8V，实际3V3与VOUT误差待真板校准。
- B1 PG12是PHY复位，不是RK复位。B2的`/RESET_n`接RK/RK806复位（1.8V域），`/MCU_RESET_n`接B1 NRST（3.3V域），只作异常恢复；`PWRON_L`人工按键与24V软开关不是同一网络，未见B1 GPIO驱动。
- 三颗双色结果灯使用PG0–PG5，显示2组LPDDR4/4X和1组eMCP；以FAIL/PASS通道定义常亮及闪烁，常温双通道闪烁、高温FAIL通道闪烁，亮/灭各500ms。实际颜色、对象顺序及复位高阻瞬态待B1验证，不以图面红绿旁注代替实物。B1的PG6/LED7已确认表示MCU正常上电，初始化时灭、默认任务完成初始化后常亮，不是RK/DDR电源或网络连通指示；另一单色灯由RK的CPU_GPIO_PWR控制。
- B1 PB3/PB4/PB5已按20260920-2134版原理图及确认贴装表配置输入、无内部上下拉；B1外部4.7kΩ上拉、B2按位贴0Ω接地，PB3最低位，PB5最高位。`board_slot_code()`返回原始0..7，111也有效；B2在位检测另走SLOT_ONLINE#。不由槽位码自动改IP、FRU、PC siteId或SPI协议，真实贴装与电平仍待验收。详见[本轮B1适配](../b1/docs/board_status_slot_20261010.md)。

原理图版本变化时先核对受影响引脚、网络和电气条件并记录原图页码。`tools/sch_netlist.py`可辅助适用的矢量PDF；不适用时使用当前可用的PDF查看/渲染工具，关键网络仍须回到原图复核。

---

## 七、待定事项（决策搁置中）

| 事项 | 状态 | 何时定 |
|------|------|--------|
| RK启动就绪、下电与取消 | 初版按本轮普通心跳就绪、直接关电；RK须确认发心跳时已初始化，取消/超时恢复仍不自动化 | B1/RK实板验收及后续业务评审 |
| DDR颗粒标志 | 0105固定mirrorIndex=0/1；失败明细由相邻9001+3001配对，未列者PASS；siteId/共轨测量顶层共享，physicalAddr为空 | RK保证0/1实际对应，MCU不按地址猜测 |
| IDD命令21..36、测量窗口、真实算法时长 | 参考文档标未实现 | RK算法接入时 |

旧Type/CRC/0x5002分歧已按用户指示由`tlv_proto`替换；多TLV打包与四TLV结果聚合已实现，旧待定表不再作为开发基线。

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
- 清串口缓冲前必须确认无在途测试；等待时间过去不等于RK已结束，超时后的RK仍可能执行。
- 新全链路脚本只适用于临时联调镜像；`tests/legacy_rk/`的旧CRC/大端命令/重传脚本不得用于现行固件。

### 内存布局
- 任务栈：rk 32KB（含心跳）/ tcp 16KB，不再创建独立hb任务；堆128KB，实际余量须以当前镜像运行时测量为准。
- OTA健康观察对象为默认任务、TCP、监测和RK共四项，B1 OFF时RK仍须推进；不得恢复已删除的HB健康要求或用RK伪报两项进度。
- 确认堆余量用 `xPortGetFreeHeapSize()`

---

## 九、提交规范（git）

- **远程仓库**：Gitea `http://192.168.1.122:3000/LILIHONG/stm32h743`（私有），本地 remote=origin，分支 main
- 提交信息描述本次变更及相关验证；项目整体进度只在任务文档中按状态变化更新，不要求每次提交重复描述。
- 提交和推送分别按本次任务的已有授权执行；已有明确授权不重复确认。暂存前检查差异，只纳入本任务文件或改动，不用全仓`git add -A`夹带用户或其他任务的修改。按逻辑完整性组织提交，不强制每任务一次commit；提交授权不自动等于推送授权。
- 认证：用户名 `LILIHONG`，密码填 Access Token（已配置 `credential.helper store` 可免重复输入）

> 注意：组织仓库 `mcu/stm32h743` 中 LILIHONG 只有读权限，push 会拒。用 `LILIHONG/stm32h743`。
