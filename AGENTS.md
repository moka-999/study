# 项目说明书（AI 接手必读）

> 本文件是给 AI 编码助手的「接手说明书」，换任何 AI 打开仓库先读这里。
> 当前进度统一维护在 [驱动任务清单](doc/driver_tasks.md#current-status)。历史记录见 [交接历史](docs/agent_handoff_history.md)，按任务需要读取。

文档按[索引](doc/README.md)选读，不默认通读历史/临时目录。`.rgignore`只控制默认搜索，追溯时显式指定路径；过时资料的[清理归档](doc/history/cleanup_20261020/README.md)可恢复，不作为当前需求或操作指令。

---

## 一、项目是什么

STM32H743 **阿波罗 V2** 板，作为 **TCP server + 双协议网关**：PC–MCU 使用固定18字节头+JSON，MCU–RK以RK工程师的`D:/pro/tlv_proto`为协议基线；MCU负责业务转换，不做原始帧透传。

**拓扑**：
```
PC ──TCP:8888──► MCU(STM32H743) ──逻辑 RK 串口──► RK3588
                                       └─ 阿波罗 V2: USART1 / B1: UART4（固定RK UART1_M0）
PC ──独立温控接口──► B2 MCU ──► 全部加热及其他底板级控制
B2 MCU（共享SPI主机）──SCK/MOSI/MISO + CS0..7──► B1 MCU ×8（各自SPI2从机，状态交换）
```

B2 MCU由其他同事开发；PC或B2本地屏幕管理温控，B1不控制、代理或转发加热。SPI以0004统一状态交换通知testerId及各镜像常温/高温显示模式，B2分别通知PC与B1。B2共享SCK/MOSI/MISO、每槽独立CS；B1 SPI2从机，V2联调使用SPI3，Mode 0、8位MSB、硬件NSS低有效、初始1MHz。协议及实现分别见[协议](doc/b1_b2_spi_protocol.md)、[接入记录](doc/b2_spi_integration_20261020.md)；离线验证不代表硬件联调通过。

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
- 测试须先0106 `{}`建立当前连接的整轮，再串行0103/0105 FUNC、0108/0109 PARA或010A/010B SLT，最后0107提交errorchip失败数组。mirrorIndex 0/1/2映射两组LPDDR/eMCP灯，未知编号整包拒绝；0701/0702/0703已从固件删除，合法旧请求回0006/notSupported。0108按FUNC格式接入16项IDD名称，0109仅MCU方向；先只向RK下发21..36编号，标准IDD窗口/判限待接。提前0107只废弃结果、不取消RK或提前解除busy；未入RK队列的FUNC/PARA不再投递。SLT固定slt/memtest86、index=null；默认空时序只准备电源/Boot，仅在RK查询时回复本轮Freq，未查询也继续，非空普通时序等本轮SPL心跳后发送，不检查EDK；同连接010B回收占用，断线不自动认领或释放SLT。0106按B2已知模式显示常温/高温闪，未知镜像灭；轮内允许B2部分更新，0107后只缓存模式、不覆盖最终结果。
- TCP解析必须支持半包、粘包和连续多包；第一组测试JSON已确认，其余命令与字段分批冻结。
- 已固定引入cJSON v1.7.19，使用FreeRTOS堆、递归深度16；完整PDU上限4096字节。
- FUNC0103、PARA0108、SLT010A均必须携带`TimingParameters`对象；默认时序传`{}`，缺失或null回invalidArgument。16个子字段可部分提供字符串：CASReadLatency/CASWriteLatency/Freq/tREFI/tWR/tRTP/tRCD/tRP/tRASmin/tRASmax/tWTR/tRFCmin/tFAW/tCCD/tRRD/BL。Freq为int64，其余为u32十进制整数；PC判定取值及适用性，MCU只检查格式/位宽并原值编码。空串/省略不下发普通时序，Freq缺省回复-1；不继承上一请求。Freq变化走既有冷启动并在0x35查询中回复，普通时序在SPL就绪后无应答逐项发送，再下发算法；发送完成不代表生效。见[接入记录](doc/rk_timing_frequency_20261105.md)。旧包补字段后必须重算Length。
- MCU升级已移除；两板恢复128KiB精简Boot（0x08000000）＋单App（0x08020000），合并文件一次烧录，不含OTA/回滚。0x0901～0x0903仅保留编号，合法旧请求回0006/notSupported，不访问Flash/RK。W25仍保留0x00410000和0x00C10000起始的两块8MiB RK空间；见[直接烧录](doc/direct_flash.md)。

### MCU–RK（2026-09-23替换后的协议）

```
[0x5A] [Len:2B BE] [HCRC:2B LE] [Seq:8B BE] [TLV...] [FCRC:2B LE] [0xA5]
```
- CRC = Modbus CRC-16（多项式0x8005，右移实现0xA001，初值0xFFFF）。HCRC覆盖前3字节；FCRC仅覆盖Seq及TLV，不含Length。
- TLV 单元：`[Type:2B BE][TLV_Len:2B BE][Value]`；Value内多字节整数为小端。
- 命令容器`0x5000`的前两字节是小端CmdID，任务发送无参数编号1..20（FUNC，17=MATS）或21..36（PARA，名称按idd_0等精确匹配）。101A动作bool（1进入/0退出）已另备内部IDD会话：发完进入帧后默认等5ms，逐点保存5s电流、冻结统计后发退出；正式任务尚不附带，需先确定判限/JSON再绑定发送完成与采样事件，见[内部准备](doc/idd_session_prepare_20261105.md)。7018寄存器地址u32LE仅编解码准备。普通心跳为Length=0、零TLV的16字节完整帧；接收仍兼容0x0000空Value。
- RK运行期上行帧填写最近收到的MCU Seq；MCU下行命令/心跳共用递增u64流水号。启动频率查询0x35独立应答5001+801C/int64，不能套用算法结果窗口；B1上电过渡只开放此查询。普通时序5000+701B/u32无应答发送，见[时序接入](doc/rk_timing_frequency_20261105.md)。
- 测试中进度帧同帧携带`0x5001`算法号u16LE和`0x3016`进度u8（0..100），各一次；先检查Seq窗口和在途算法号，单独0x3016无效。MCU每秒主动发心跳，当前RK收帧后空闲回零TLV心跳、在途回进度，最终结果可主动发送；MCU不再应答RK心跳。允许窗口内Seq滞后及多帧同Seq；进度仅参考，不作为完成/ready或自动恢复依据，不增加PC JSON字段。
- 最终结果为单个3/5/7 TLV包：5001算法u16LE、1017整体FAIL位u8、9002耗时u64LE各一次；0～2组相邻9001地址u64LE＋3001 die u8。9001=0表示该颗PASS，非零才计入失败数组；die仅0/1且唯一（PASS明细也查重），1017须与是否存在非零失败地址一致。三个标量和明细对可换序，地址后须立即跟所属die；旧四详情/独立FAIL不再拼接完成。
- 帧内TLV数量为0..16；Length仅允许0或4..1024，1..3仍非法。新合同及验证见[增量记录](doc/rk_protocol_update_20260930.md)。
- 状态机 5 态：IDLE→HEADER→DATA→CRC→FOOTER；仅用于MCU–RK串口链路。

> 旧PC–TLV `dispatch.c/.h`已删除。RK参考工程中的POSIX/UART演示代码不直接移植，复用本工程串口/FreeRTOS并补齐边界。V2正式0103支持用户确认的外部已启动模式：power/bootMode均null、reboot=false，可先发普通时序再执行FUNC/PARA算法，非负Freq回invalidState，不接管电源/启动；默认不设算法等待超时。B1也已接入该模式，但额外要求真实电源状态ON，测量用实际采样。B1完整上电/冷重启已装载可调初值，采用本轮普通心跳就绪与直接关电；训练安全范围交由PC确认，MCU仍校验TPS范围和步进。默认OFF，V2真实电源配置仍空；见[当前业务配置与验收](doc/b1_power_business_20261013.md)。`ENABLE_RK_GATEWAY_TEST`仅保留旧隔离模拟用途，真实RK接入不打开该开关。外部已启动模式见[真实RK接入](doc/rk_real_test_20260928.md)及[B1同步记录](../b1/docs/real_test_sync_20260929.md)。

V2可显式启用`ENABLE_V2_POWER_SIMULATION`：沿用上述正式任务及真实RK，仅电源/Boot/测量改用内存模型，PC响应不附加`simulation`字段；0601在测试中只读且不因算法在途回busy。是否模拟以构建配置与安装记录为准，不能因无标记就认作实测。默认关闭、B1禁止，与旧网关联调开关互斥，不作为真实电源/IDD验收。详见[V2电源模拟](doc/v2_power_simulation_20260928.md)。

---

## 三、目录结构

| 目录 | 内容 |
|------|------|
| `App/protocol/pc/` | PC–MCU独立字节流状态机、JSON/命令分发、固定头响应组帧和PC业务JSON辅助层 |
| `App/protocol/rk/` | RK线格式组帧/TLV状态机，详情、进度和FAIL报告解码 |
| `App/protocol/b2/` | B2独立帧/TLV校验、CS事务状态机和应答编码，不依赖RK模块 |
| `App/protocol/common/` | 无状态通用CRC算法；不含任何链路的Seq或业务 |
| `App/tasks/pc/` | PC侧TCP server任务；负责socket生命周期和传输编排，业务由PC命令表分发 |
| `App/tasks/rk/` | 单个RK任务统一推进串口、请求/最终结果和每1秒心跳；不自动重发测试 |
| `App/tasks/b2/` | 独立B2任务及请求/应答队列；IP和整轮灯态由TCP上下文顺序应用，不混入RK任务 |
| `App/tasks/common/` | LED、采样监控等不隶属单一通信对象的正式任务 |
| `App/tasks/test/` | Mock、自检和逻辑分析仪波形任务；不代表生产业务 |
| `boot/` | 独立精简Boot、FRU准备和跳转；由本工程合并构建，无OTA |
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

### 直接烧录（SWD）

2026-10-28起B1/V2为精简Boot＋单App：Boot位于`0x08000000`占128KiB，App位于`0x08020000`占1920KiB；最终ELF/BIN合并两者，不生成A1/A2或`.ota`。两工程各自Ctrl+Shift+B自动编译、检查、备份内部Flash、直接写入BIN并回读、复位运行；空白板和原OTA板使用同一入口。按快捷键即确认RK/温控空闲、PC断开并允许MCU复位。B1复位仍在RAM初始化前关闭RK/DDR。

```powershell
# 只构建，不访问设备
.\tools\dev.ps1 build
# B1直接烧录；V2不带模拟参数时构建实际驱动配置
.\tools\dev.ps1 flash -ConfirmIdle YES
# V2日常联调与默认快捷键配置
.\tools\dev.ps1 flash -ConfirmIdle YES -PowerSimulation -MatsFailTest
```

产物位于`build/standalone/Debug/`（可选Release），V2为test.elf/bin，B1为b1.elf/bin。版本不自动递增，可显式指定-Version；多探针指定-Serial。各工程自带`tools/direct_flash.py`，不依赖同级Bootloader。默认SWD Normal/SWrst/950kHz，不要求B1 J5引出NRST；不改Option Bytes、不全擦、不操作W25。原OTA板会替换为精简Boot＋单App，不再回滚。下载工具不访问W25；有器件时Boot在复位后维护FRU。失败保留备份，不自动重试。工厂GUI及接线见[直接烧录说明](doc/direct_flash.md)。

### 工具链路径
```
C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/
```

### 串口 / 网络
- 串口：2026-09-23实测COM5 = CH340 → USART1（阿波罗阶段接PC模拟RK3588）；每次先枚举确认，不硬编码旧COM3。
- 当前测试MCU IP：`192.168.1.201:8888`，继续使用本地配置。
- 正式B1 IP由B2 MCU通知testerId，再与槽位码加1组合：PC=`192.168.210.9`，B2末段=`10*T`，B1末段=`10*T+S`（T=1～21，S=1～8）。10号/18号B2分别为`.100`/`.180`。0004收到不同tester/slot后在无已接受PC连接、无测试/电源在途时动态切换，否则整包busy；同IP不重配。联调掩码255.255.255.0、网关0.0.0.0，RAM保存、不改FRU；首次配置前保留本地测试地址。真实八槽上线、首次发现及默认IP冲突隔离仍待系统验收，见[需求基线§1.1](doc/requirements_baseline.md)。

### 崩溃诊断（HardFault 排查）
- 诊断地址随链接变化，读写前必须从**实际烧录的同一份ELF**用`arm-none-eabi-nm -S -n`查询`crash_log`（16×uint32）和`tcp_diag`（8×uint32）。禁止把历史地址的`-w32`命令直接套用到新镜像。`.crash_log`为NOLOAD，复位不清零。
- 反汇编：`arm-none-eabi-objdump -d --start-address=<pc-0x100> --stop-address=<pc+0x100> test.elf`
- **判断异常栈**：用 `EXC_RETURN(LR) bit2`（1=PSP线程 / 0=MSP handler），**不要**用 `__get_CONTROL()&2`（进异常时硬件已清 SPSEL，判断必错）

### Boot＋单App启动与向量

- Boot向量位于`0x08000000`；App向量位于`0x08020000`，`SCB->VTOR`使用`g_pfnVectors`。CubeMX重生成后核对实际链接位置，不使用手写固定偏移。
- B1的`Reset_Handler`在RAM初始化前拉低PC6/PC7/PG11；不能因删除MCU Boot而删除耦合复位关电。
- 诊断用实际烧录同份ELF的符号；硬件运行及Option Bytes仍需实板核验。

### 运行测试
```bash
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 protocol-test  # 实际C帧/状态机/事务的QEMU回归
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 style-test     # 自定义层边界/事件模型+纯接口头独立编译
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 pc-protocol-test # PC固定头/cJSON离线合同
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 b2-protocol-test # 实际C SPI协议/状态机/事务，不访问硬件
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
5. **正式业务门禁**：外部已启动模式只下发FUNC/PARA算法编号；B1完整电源/Boot路径采用可调初值，PC确认训练安全范围，MCU校验TPS编码并等待本轮有效普通心跳，保留notReady、互斥及故障处理。0105/0109每算法一包、两颗mirrorIndex=0/1，siteId/measurementData顶层共享；缺失编号/测量如实留空。断线不取消算法，新测试/电源改动在途busy，handler不阻塞等结果。V2显式MATS假FAIL只改第一颗PC视图，第二颗真实；B1禁止。
6. **D-Cache 已关** → 无 cache 一致性顾虑，**勿再加 `SCB_InvalidateDCache_by_Addr`**（否则 UsageFault 升级 HardFault）。
7. **DMA 够不到 DTCMRAM**（DMA 在 D2 域），DMA 缓冲放 AXI SRAM (RAM, 0x24000000)。
8. **FreeRTOS 堆(128KB)手动挪到 AXI SRAM**（`heap_4.c` 的 `.heap` section + linker），不是 CubeMX 默认位置。
9. **SPI按板型分配**：Apollo V2的SPI2仅给AD7124开发（Mode 3），B2链路另用SPI3（PA15/PB3/PB4/PB5）；B1用SPI2从机接B2、SPI4主机接AD7124。B2任务独立于RK，DMA1 Stream3/4与`.b2_dma` AXI缓冲专用，禁止占用UART的Stream0～2。
10. **自定义接口归属**：总线实例统一由`board_interfaces.h`声明，UART内部钩子只在`uart_hw.h`；RK任务用`rk_link_next_sequence()`分配独立序号，禁止外部直接修改计数器。`uart_start_all()`在调度前初始化TX互斥锁；分配失败拒绝发送。注释/风格及剩余耦合约束见`doc/code_quality_review.md`。
11. **RK解析错误必须上送调用方**：tlv_sm_feed完成后还须检查tlv_sm_get_frame；负数失败、0为零TLV心跳。Length为0或4..1024，最多16 TLV；底层保留未知TLV，最终业务只接受3/5/7合同，不能部分交付。tlv_unpack输入从5A起的完整帧和真实长度。V2/B1字节间门限均100ms，连续完整帧无需外部reset。
12. **信息类响应的数据源**：已装W25时0101/0102加载其FRU，返回测试仪名称、硬件版本及测试板名称；testerVersion不是MCU固件版本。FRU首次初始化/旧格式迁移由Boot完成，正式App只读。W25装配按目标板分别确认，不能按IP推断；当前B1验板目标未焊器件，`board_storage_config.h`默认`BOARD_W25_PRESENT=0`，Boot和App跳过器件、信息使用常量。已装器件板须改1并重编、下载完整Boot＋App；V2仍默认1。启用器件时初始化/FRU失败会停在Boot，不能因串口无输出就绕过真实故障；镜像及验收状态见任务清单。App启动不读写固件版本、镜像CRC或升级状态；旧版本区、MCU包/启动日志和RK预留地址均不访问或擦除。详见[Flash布局](doc/flash_layout.md)。

---

## 六、任务入口与硬件核对

当前进度、下一步和各层验证状态统一维护在 [驱动任务清单](doc/driver_tasks.md#current-status)。状态变化时更新该文件；本文件只维护稳定约束与入口，不继续追加里程碑。

Word文档的最新版保持固定文件名，历史版本改名归档；PC协议固定为`doc/pc_tcp协议.docx`。修改前按文件最后修改时间保存到`doc/history/pc_tcp/pc_tcp协议_yyyyMMdd_HHmmss.docx`，同秒重名加序号，不覆盖历史文件；具体规则见`doc/README.md`。不得因默认渲染工具缺失就宣称Word无法检查；可用本机WPS后台只读导出后检查实际分页，并如实记录使用的渲染方式和检查范围。

对外PC协议Word只保留PC工程师需要的请求示例、字段含义、响应关系和操作规则，说明简短；MCU内部流程、硬件时序推导、历史及开发状态留在Markdown内部文档中，不整段同步到Word。精简不能删掉PC必须遵守的安全电压、重启、index及最终结果推进规则。

按任务类型完成适用检查，软件、文档或工具任务可独立交付；缺少真板时单列硬件验收待办，不把离线/Mock/构建通过写成真机通过。具体流程见 [开发阶段与流程](doc/driver_tasks.md#execution-workflow)。旧里程碑、当时的待办和原理图摘录保存在 [交接历史](docs/agent_handoff_history.md)，其中“本轮/尚未/当前”仅指记录日期。

修改板级驱动前核对 [任务清单的引脚总表与器件章节](doc/driver_tasks.md#board-pinout)。电源控制已按 `D:\pro\MYZR-RK3588-ZUOYI_SCH_20260831-.pdf` 复核，证据、适用范围与待验收项见 [B1电源控制记录](doc/b1_power_control_review.md)；仓库0811原图仅为旧版参考，不得覆盖新结论。本次电源核对不批准IP方案或其它待定业务。

关键硬件边界：

- B1 UART4（PA11 RX/PA12 TX）固定经RS0102 U4008对接RK UART1_M0（GPIO2_B6 RX/GPIO2_B7 TX）跑协议；20260920-2134原理图第6/14页已复核，旧记录的UART0_M0为误记，网络名UART_RX_M0中的M0不代表UART0。USART2（PD5/PD6）经U4013双向连接RK UART2_M0_DEBUG，USART1（PA9/PA10）输出所选MCU/RK日志。MCU模式处理本地命令，`log rk`加换行后转发输入；RK模式只截获行首完整`log mcu`加换行返回MCU，控制字符原样转发，切换不写Flash。协议口115200/8N1及双向电气仍待真板验收，Debug可用不代表协议线路已通。V2的USART1仍是协议口，不能照搬B1日志分配。
- PC6控制`VCC4V0_SYS`，同时给RK806和三颗TPS供电；PC7控制独立的`VCC3V3_SYS`，不能作为暂停RK训练的开关。0831图PG11到TPS EN的串联位为NC，TPS经10kΩ上拉随4V启动，不能把PG11动作当成真实三轨开关。MCU由独立`3V3_MCU`供电，可先运行再启动RK。按用户确认初版直接关电，不等待RK关机应答，故障强制关断保留；这不保证RK写盘已结束。顺序/延时及RK806复位释放前的调压窗口仍须真板验证，旧波形不是批准值。
- B1初值集中在`../b1/Core/Inc/board_power_config.h`：PC7先开→10ms→PC6开→5ms后写TPS；反序下电间隔10ms；OFF停留500ms、Boot稳定10ms、切换上限2s、RK启动心跳上限60s。main绑定心跳适配器但保持OFF，V2真实电源配置未启用。初值不是硬件验收；PC负责DDR安全范围，MCU只校验TPS物理范围/步进；直接关电不等待RK写盘完成，见`doc/b1_power_business_20261013.md`。
- `power`表示DDR初始化/训练目标，不能偷换为训练后拉偏。底层训练目标与运行拉偏缓存分离；业务通过`monitor_request_training_power_on`在OFF时选择目标并上电，默认/只选类型时仍为标称。用户接受“天然启动窗口内赶得上”作为开发预设，非硬件验收结论；寄存器可写范围不等于安全训练范围。新PC字段、MCU安全校验、配置变化强制冷重启和RK-ready闭环见任务清单，不绕过缺失的配置门禁，不预设必须依赖B2。
- TPS三轨地址为0x49/0x41/0x4F，分别使用I2C1/I2C2/PH4-PH5软件开漏I2C。PAC1934同用该软件总线，7位地址0x16（总线字节0x2C/0x2D）；20260920-2134版B1三路分流电阻各0.02Ω，B1板级换算已跟随更新，V2测试配置仍为0.01Ω，不能无条件覆盖两板参数。
- 0103.power/0509使用`vdd1/vdd2/vddq`十进制字符串直接表示绝对mV，不再接收旧steps字段。共享解析器严格校验`valMv=nominalMv+stepMv*n`及TPS范围，n为整数；VDD1步进10mV、VDD2/VDDQ步进5mV，不取整。2026-11-04起B1的0509在OFF只保存下次上电的RAM训练目标（允许非标称、不访问TPS），0501上电写入；ON同类型实时调压不覆盖训练目标，MCU复位恢复标称。V2未同步该OFF语义；010x流程不变。内部复用原生档位及驱动，可写不等于DDR安全。范围、兼容与验证见`doc/pc_voltage_mv_20261012.md`。
- B1 AD7124使用SPI4 PE11–PE14、Mode 3，实板ID=0x07符合AD7124-4白名单0x04/0x06/0x07。当前首板用AVDD基准，内部REF_EN关闭；`../b1/Core/Inc/board_adc_config.h`集中选择和实测3333mV换算。V2默认仍用内部2.5V。保持增益1、无缓冲及约240ms三轨扫描；AVDD方案精度待实测，内部基准方案须先补齐REFOUT去耦。分析仪CPHA标签有歧义时查看原始上升采样沿，不据标签改为Mode 2。
- MCP4725在I2C2、地址0x60，Boot目标电压0–1.8V；不得超过1.8V，实际3V3与VOUT误差待真板校准。
- B1 PG12是PHY复位，不是RK复位。B2的`/RESET_n`接RK/RK806复位（1.8V域），`/MCU_RESET_n`接B1 NRST（3.3V域），只作异常恢复；`PWRON_L`人工按键与24V软开关不是同一网络，未见B1 GPIO驱动。
- 三颗双色结果灯使用PG0–PG5，显示2组LPDDR4/4X和1组eMCP；以FAIL/PASS通道定义常亮及闪烁，常温双通道闪烁、高温FAIL通道闪烁，亮/灭各500ms。实际颜色、对象顺序及复位高阻瞬态待B1验证，不以图面红绿旁注代替实物。B1的PG6/LED7已确认表示MCU正常上电，初始化时灭、默认任务完成初始化后常亮，不是RK/DDR电源或网络连通指示；另一单色灯由RK的CPU_GPIO_PWR控制。
- B1 PB3/PB4/PB5配置输入、无内部上下拉；B1外部4.7kΩ上拉、B2按位贴0Ω接地，PB3最低位。`board_slot_code()`返回0..7，111有效；加1形成siteId，B2在位检测另走SLOT_ONLINE#。仅在B2配置成功后将该siteId用于运行IP及PC结果，不写FRU；后续0004重新读取槽位，变化仍受改IP的busy门禁。真实贴装与电平待验收，见[原引脚核对](../b1/docs/board_status_slot_20261010.md)和[最新接入](doc/b2_spi_integration_20261020.md)。

原理图版本变化时先核对受影响引脚、网络和电气条件并记录原图页码。`tools/sch_netlist.py`可辅助适用的矢量PDF；不适用时使用当前可用的PDF查看/渲染工具，关键网络仍须回到原图复核。

---

## 七、待定事项（决策搁置中）

| 事项 | 状态 | 何时定 |
|------|------|--------|
| RK启动就绪、下电与取消 | 初版按本轮普通心跳就绪、直接关电；RK须确认发心跳时已初始化，取消/超时恢复仍不自动化 | B1/RK实板验收及后续业务评审 |
| DDR颗粒标志 | 0105固定mirrorIndex=0/1；失败明细由相邻9001+3001配对，未列者PASS；siteId/共轨测量顶层共享，physicalAddr为空 | RK保证0/1实际对应，MCU不按地址猜测 |
| IDD进入/保持/退出、有效测量窗口、判限及真实算法 | 21..36编号和0108/0109已接；101A/7018仅编解码准备；全程统计不等于标准窗口 | RK算法实板联调时，见[PARA接入](doc/para_integration_20261026.md) |

旧Type/CRC/0x5002分歧已按用户指示由`tlv_proto`替换；多TLV打包与四TLV结果聚合已实现，旧待定表不再作为开发基线。

---

## 八、已知坑（改代码前必读）

### CubeMX 重新生成会覆盖的手动改动
1. 自定义源/头 → 放**顶层 CMakeLists.txt**（勿放 cmake/stm32cubemx/）
2. `heap_4.c` 的 `.heap` section → 重加；长期用 `configAPPLICATION_ALLOCATED_HEAP=1`
3. `stm32h7xx_it.c` 的 `HardFault_Handler_C` → USER CODE 区恢复
4. USART TX drain → 已用 HAL `TxISR` 钩子根治，it.c 无自定义代码
5. **`system_stm32h7xx.c`**：两板VTOR使用实际`g_pfnVectors`；App向量为0x08020000，CubeMX重生成后不得恢复独立App首地址或手写偏移。
6. **`STM32H743xx_FLASH.ld`**：`BOOT=0x08000000/128K`、`FLASH=0x08020000/1920K`，入口`Boot_Image_Reset`；保留AXI堆、DMA、ETH、崩溃段和实际镜像边界符号。
7. **`STM32H743xx_FLASH.ld`**：`.rodata`中的`KEEP(*(.firmware_version))`必须保留，否则未接入PC查询前版本元数据会被`--gc-sections`回收

> ✅ 已入 .ioc 自动保留：`SPI2.CPolarity=HIGH + ClockPhase=2EDGE`（**AD7124 模式 3** CPOL=1/CPHA=1，手册 P8 明确"operates in SPI Mode 3"，20260819 修正——此前误判为模式1）；`freertos.c` StartTestTask 自检在 USER CODE 区；顶层 CMakeLists 不受 CubeMX 影响。

### 测试脚本坑
- 清串口缓冲前必须确认无在途测试；等待时间过去不等于RK已结束，超时后的RK仍可能执行。
- 新全链路脚本只适用于临时联调镜像；`tests/legacy_rk/`的旧CRC/大端命令/重传脚本不得用于现行固件。

### 内存布局
- 任务栈：rk 32KB（含心跳）/ tcp 16KB，不再创建独立hb任务；堆128KB，实际余量须以当前镜像运行时测量为准。
- 看门狗启用时监测默认任务、TCP、监测和RK四项存活，B1 OFF时RK仍须推进；它不再承担升级健康确认，不阻塞PC业务等待30秒。B1的`Core/Inc/board_watchdog_config.h`现默认`BOARD_IWDG_ENABLE=0`供验板，不启动IWDG或喂狗任务；设1重编可恢复，V2不变。该宏不修改Option Bytes，也不能停止旧固件已启动的IWDG，首次下载仍须处理暂停时的调试冻结。
- 确认堆余量用 `xPortGetFreeHeapSize()`

---

## 九、提交规范（git）

- **远程仓库**：Gitea `http://192.168.1.122:3000/LILIHONG/stm32h743`（私有），本地 remote=origin，分支 main
- 提交信息描述本次变更及相关验证；项目整体进度只在任务文档中按状态变化更新，不要求每次提交重复描述。
- 提交和推送分别按本次任务的已有授权执行；已有明确授权不重复确认。暂存前检查差异，只纳入本任务文件或改动，不用全仓`git add -A`夹带用户或其他任务的修改。按逻辑完整性组织提交，不强制每任务一次commit；提交授权不自动等于推送授权。
- 认证：用户名 `LILIHONG`，密码填 Access Token（已配置 `credential.helper store` 可免重复输入）

> 注意：组织仓库 `mcu/stm32h743` 中 LILIHONG 只有读权限，push 会拒。用 `LILIHONG/stm32h743`。
