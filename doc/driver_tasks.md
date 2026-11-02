# MCU 当前进度与验收清单

更新：2026-11-02。本页只维护当前状态；已完成过程、旧方案和历史测试数字按需从主题记录读取。[精简前原文](history/cleanup_20261020/README.md)可完整恢复。

<a id="current-status"></a>

## 当前进度与下一步

| 模块 | 当前代码状态 | 已有验证与剩余边界 |
|---|---|---|
| B1已装W25 | 用户确认MCU侧W25Q256已焊，默认启用器件；Boot维护FRU、App只读，无OTA | 201/202完整镜像重建、实际C/模拟Flash的1280个写入中断点及保护回归通过，未烧录，真实QSPI及首启待验收，见[记录](b1_debug_bridge_20261102.md#同轮启用已焊w25q256) |
| B1 RK双向调试 | `log rk`后USART1输入经USART2发送RK DEBUG；行首`log mcu`返回本地，UART4协议独立 | 201/202构建、结构检查及MCU/RK两种默认模式实际C回归通过，未烧录；完整check存在旧Flash断言失败，见[记录](b1_debug_bridge_20261102.md) |
| B1首板灯态排查 | 核对高电平点亮；B1未发现网络成功闪灯分支，PG6初始化后常亮，结果灯只按整轮/B2模式推进 | 14:59实板仍states=0,0,0、PG0～5低且PG6高；不能据单点快照解释实物闪烁，待核对灯位/RK独立灯及波形，不改显示规则，见[记录](b1_led_diagnostics_20261102.md) |
| B1 AD首板排查 | B1接地输入策略已实板完成校零并产生三路读数；运行仅关闭负端UV，校零临时关闭正端UV后回读恢复，其它错误仍拒绝 | 14:59三轨1669/1016/552mV；用户测AIN为1.783/1.085/0.590V、REFOUT确认2.6V，精度未通过。原理图及现有BRD显示REFOUT侧未见0.1µF去耦，C4120被NC的FB2306隔开，待硬件核实并复测；不认定全部偏差已归因，本轮未修改固件或烧录，见[记录](ad7124_b1_startup_fix_20261102.md) |
| 采样JSON小数输出 | B1/V2补齐newlib-nano浮点格式化及回读支持，避免小数变成空JSON数字；字段不变 | 实际C在nano库下修复前失败、修复后两板通过；201/202/V2重建完成，未烧录，待连续0601实板复测，见[记录](json_float_fix_20261102.md) |
| PC TCP | 18字节头+cJSON、状态查表和命令查表；半包/粘包、异常输入恢复、独立发送Seq；三类开始包必填TimingParameters对象，读/写延迟分别用CASReadLatency/CASWriteLatency | V2 0.1.39已安装；33项实板参数解析/整轮门禁检查及心跳、信息、采样查询通过。时序仍只存RAM，尚未下发RK或实测生效 |
| 整轮/FUNC/PARA/SLT | 0106开始整轮；0103/0105 FUNC；0108/0109 PARA；010A/010B整次SLT；0107失败数组收尾；070x删除 | PARA的16项IDD编号下发及0109通道已同步两板并随V2 0.1.39安装，实际C回归/构建通过；本轮未启动真实RK算法，IDD条件/窗口及真实SLT下载待联调，见[PARA接入](para_integration_20261026.md) |
| RK链路 | 独立任务、1秒心跳；双TLV进度；单包3/5/7 TLV双颗粒结果；在途事务和Seq窗口 | V2真实RK链路已有阶段记录，实际C回归通过；B1 UART4/RS0102电气验收待板，不宣称所有算法/负载已验收 |
| B1双板调试地址 | 构建参数`-TestIpLastOctet`选择本地IP末段；默认201，第二块202使用独立目录和带IP文件名的完整ELF；保留UID派生MAC及B2动态IP | 两份Boot＋App构建、地址及实际IP指令检查通过，0/255越界拒绝；用户已烧录旧201，启动时基问题修正后两份均重新构建，待重烧及双板实测，见[用法](direct_flash.md#11-两块b1同时调试) |
| B1启动时基 | 补齐TIM1的HAL计时回调，解决PHY复位延时不能结束；V2已有回调、不修改 | 用户重烧后已进入RTOS/lwIP并输出状态，TCP曾完成查询；只读确认任务计数推进。后续发现TCP错误码混用，见[启动记录](b1_hal_tick_fix_20261102.md)及[断线修正](b1_tcp_errno_fix_20261102.md) |
| TCP空闲断线修正 | B1/V2任务改用lwIP的errno，避免正常接收等待被newlib另一份错误码误判为断线；协议不变 | 旧对象回归失败、修复后三个构建检查通过；B1 201/202及V2已重建，尚未安装本次修正，空闲后查询及持续连接待实板验收，见[记录](b1_tcp_errno_fix_20261102.md) |
| B2链路 | 独立B2任务/协议/CS状态机，SPI DMA，0004状态交换、动态IP及镜像模式显示；已同步双SPI实测后的DMA错误分类和诊断 | 两板实际C/PC整轮/busy、构建及OTA双槽回归通过；2026-10-21已安装V2 0.1.38，SPI3及接收队列初始化确认。正式镜像本轮没有SPI主机请求；B1八槽、电气与负载仍待验收，见[恢复安装记录§9](b2_spi_integration_20261020.md#9-v2恢复与0138安装2026-10-21) |
| 第二块V2主机 | `tools/b2_spi_master`裸机USART1+SPI3，只有query/id/mode | 已构建、编解码离线通过；未烧录/互联，不包含OTA、网络或采样 |
| 单板双SPI联调 | 同级`../b2_spi_loopback`完整独立Flash工程；SPI2主机→SPI3从机，真实lwIP、板载灯及协议日志；测试主机/串口CLI不加入B1/V2正式构建 | 查询、坏CRC恢复、重复Seq应答已有实板日志；2026-10-21用户确认双SPI及实际IP切换成功。DMA修正已同步正式两板；灯态、跨板时钟/复位和八槽仍分别验收，不能由换IP成功推定 |
| B1电源/Boot | 正式0103/0108与050x接真实驱动、可调初值、配置变化冷重启、本轮普通心跳就绪和直接关电 | 实际C故障/边界回归及构建通过；天然训练窗口、供电/放电时序、DAC输出待B1 |
| 图形验板工具 | `tools/b1_debug`复用050x/060x，单步控制、只读持续监测/曲线、TPS原生范围电压清单、AD电压/PAC电流CSV及无SPL的Boot上下电循环；独立EXE，不改固件或协议 | 25项回归、GUI本机TCP回环及两个窗口尺寸检查通过；监测停止不下电、无效/过期读数留空。未访问开发板；Boot缓存不能证明RK实际介质，见[用法与边界](../tools/b1_debug/README.md) |
| TPS/PAC/AD7124 | 三轨调压、真实采样、错误传播及恢复已接监测层；B1使用20mΩ分流；AD初始化逐路校零后才采样 | 已有实际C/Mock及部分V2发送波形验证；新增初始化校准回归见[采样调整](sampling_schedule_20261026.md)，没有真实器件精度或B1上电验收 |
| 采样统计 | 电流快速独立读取，AD保留约240ms高精度三轨扫描；内部µA分组统计，0602只读快照，0105/0109冻结本次统计 | 两板实际C/PC回归、构建和OTA双槽验证通过；已随V2 0.1.39安装，0602响应正常。V2启用500ms内存模拟，不能据此验收真实采样持续速率/噪声/精度；见[采样调整](sampling_schedule_20261026.md)，IDD条件/窗口/限值仍待确认 |
| LED/槽位/日志 | PG6初始化后常亮；PB3～5读取0..7加1；三组结果灯；B1本地切换MCU/RK日志，新增启动阶段和status只读诊断 | B1构建、板级及实际C诊断/两种日志默认/监测回归通过；首板已输出启动日志，暴露PHY复位前时基遗漏。运行期诊断、真实颜色、槽位、电气及日志负载仍待验收，见[首板诊断](../../b1/docs/board_diagnostics_20261027.md) |
| W25 / Boot＋单App | 恢复128KiB精简Boot，App在0x08020000；合并ELF/BIN维持一键SWD。Boot维护FRU、App只读；B1未焊W25默认跳过并用宏定义回复0102。无OTA、版本同步或回滚 | 两板构建、地址检查、1280断电点FRU回归及无W25查询检查通过；硬件启动/实际下载待验收，见[修改记录](minimal_boot_20261028.md) |

### 软件与板上版本分开记录

- 2026-10-28恢复精简Boot＋单App，Ctrl+Shift+B仍合并直接烧录，不打包OTA或等待30秒。B1保留最早期关电、无W25首板配置；V2保留默认模拟/MATS第一颗假FAIL。操作见[直接烧录](direct_flash.md)。本轮未访问硬件，以下0.1.39是上次安装记录。

- 2026-10-26新增PARA 0108/0109、IDD编号21～36，以及仅编解码准备的101A动作/7018寄存器地址；两板源码、离线回归及构建通过。三类开始包的TimingParameters为必填对象，默认{}；CASReadLatency/CASWriteLatency独立保存，不再接受CASLatency。旧包须补字段并重新计算长度。本地Word/Markdown及飞书已对齐，见[接入记录](para_integration_20261026.md)。
- 2026-10-26采样调度、µA统计及AD初始化三路校零已同步两板并通过相关回归、构建及双槽验证；随V2 0.1.39安装，但V2使用内存模拟，真实采样仍待B1验收。
- 2026-10-26最后实测V2为`0.1.39/A1/confirmed`，TCP实查包标识`3295504F`、逻辑电源OFF，地址为`192.168.1.201:8888`；本轮未启动RK算法，ST-Link和TCP连接已释放。备份、从回环恢复及升级证据见[日常下载记录](ota_daily_download.md#v2恢复与0139安装2026-10-26)。
- 2026-11-02用户已烧录旧B1 `.201` ELF；日志停在PHY复位并约33秒重启。已补齐TIM1的HAL时基回调，重新构建 `.201`/`.202` 并通过离线回归；修正镜像待重烧验证。普通构建版本0.1.0不是安装确认，本轮未提交或推送仓库。
- V2日常任务保留电源/测量模拟和MATS第一颗假FAIL；B1使用真实驱动、不带假FAIL。必须按构建及安装记录判断，不能从没有`simulation`字段推断为实测。

### 下一步

1. 待空闲时用新直接下载入口验证V2 Boot跳转、复位及只读FRU查询，再用[两板主机](../tools/b2_spi_master/README.md)在正式TCP/RK/采样任务同时运行时复测B2，验收独立时钟/复位及总线电气。原单板回环的SPI2主机和串口CLI没有带入正式镜像。
2. 验收PC已连接、测试/电源在途时改IP的busy，断开后以新Seq重试；检查ARP、新IP连接、MISO未选中高阻及DMA/任务余量。
3. 验收新直接下载：内部Flash备份/镜像回读、旧OTA板替换为精简Boot后正常启动、无W25空白B1首次下载；不再做OTA安装/回滚验收。板型和模拟配置必须区分。
4. B1首板先重烧时基修正镜像，确认启动进入RTOS/lwIP、不再循环复位，再检查PHY链路及TCP；随后逐项验收默认OFF、总线和芯片ID、电压/电流标定及高速采集的持续速率/噪声（含PAC SLOW电平）、Boot/训练窗口、RK链路、B2八槽。
5. IDD编号下发已接；继续与RK联调实际算法，并确认进入/保持/退出、有效测量窗口、`TimingParameters`寄存器操作和RK升级方式；已准备的TLV和已存时序参数不等于已经生效。
6. 等B2/PC联合验收首上线发现与多板初始`.201`隔离、模式刷新及失联操作流程。

<a id="execution-workflow"></a>

## 开发与验收流程

- 改驱动：核对受影响的原理图/官方手册，跑实际C和相关构建；电气/精度/启动问题必须补实板证据。
- 改协议或工具：跑对应实际C、脚本或本机回环检查；不为纯文档整理额外连接硬件。
- 分开记录源码实现、离线/Mock、V2实板、B1/B2实板四层状态；未实测项保留待办。
- 详细修改写入对应主题Markdown；本页只替换当前结论，不继续叠加“之前尚未、后来已完成”。
- 暂存/提交/推送及烧录分别按本轮授权执行，不夹带其他工作区改动。

## 1. 基础设施与任务边界

| 对象 | 当前入口/边界 |
|---|---|
| PC | `App/tasks/pc`负责socket和传输，`App/protocol/pc`负责帧/JSON/命令业务 |
| RK | `App/tasks/rk`统一串口、在途结果及心跳，不创建独立HB任务 |
| B2 | `App/tasks/b2`独立收发；队列交TCP上下文顺序应用IP/灯态，不混入RK |
| 通用CRC | `App/protocol/common`只有无状态CRC，不共享Seq或链路状态机 |
| 监测/灯 | `App/tasks/common`；纯测试任务在`App/tasks/test`，不能当正式业务 |

保留DMA缓冲在AXI SRAM、D-Cache关闭及128KiB RTOS堆约束。新B2任务增加的栈/堆余量须在真实负载下测量。完整调用链和验证见[B2接入](b2_spi_integration_20261020.md)。

## 2. 电源时序

PC6控制RK806/TPS共享4V，PC7控制系统3.3V；PG11串联NC不能当作实际DDR总闸。默认OFF。B1初值为PC7开→10ms→PC6开→5ms后写TPS；正常反序下电间隔10ms，故障立即关闭。

业务初值：OFF停留500ms、Boot稳定10ms、切换上限2s、RK启动普通心跳上限60s。初版直接关电，不等待RK写盘完成；这些是用户接受的调试初值，不是JEDEC或硬件验收结论。详见[B1业务配置](b1_power_business_20261013.md)和[电源依据](b1_power_control_review.md)。

## 3. TPS三轨调压

| 轨 | 器件/因子 | 7位地址 | 原生步进 | TPS可写范围 |
|---|---|---|---|---|
| VDD1 | TPS628692A / 2 | 0x49 | 10mV | 800～3350mV |
| VDD2 | TPS628681A / 1 | 0x41 | 5mV | 400～1675mV |
| VDDQ | TPS628681A / 1 | 0x4F | 5mV | 400～1675mV |

PC发送十进制字符串`vdd1/vdd2/vddq`，不再使用steps。标称VDD1/VDD2为1800/1100mV，VDDQ随LPDDR4/4X为1100/600mV。PC负责安全范围和组合，MCU检查硬件范围/整步并精确设置；可写范围不等于DDR安全范围，禁止自动遍历。`power`是训练目标，不偷换成训练后拉偏。

料号、VSET电阻及供电选路仍以最新BOM/实物核验；旧0831图面不能当作已整改证明。见[电压合同](pc_voltage_mv_20261012.md)和[原生步进记录](ddr_native_steps_20261012.md)。

## 4. PAC1934电流

逻辑I2C4软件开漏总线，7位地址0x16；前三通道测共轨电流，CH4未用。B1分流电阻20mΩ，V2测试参数10mΩ，不能无条件互相覆盖。已接快照和窗口统计，最大值是采样点最大值，不代表捕获所有瞬态峰值；共轨测量不能区分两颗DDR各自电流。见[驱动审查](driver_review_and_changes.md)及[PAC回归](test_reports/pac1934_driver_20260921.md)。

## 5. AD7124电压

B1 SPI4 PE11～14、Mode3；V2临时SPI2。按AD7124-4和ID白名单0x04/0x06/0x07实现，内部2.5V基准、AIN0/1、2/3、4/5差分对。初始化对三个setup逐路内部校零，增益1保留出厂GAIN，全部成功才连续采样；失败沿用失效及重试流程。具体封装/BOM、真实ID、输入配置与整板标定仍须真板确认。见[采样与校准接入](sampling_schedule_20261026.md)、[AD回归](test_reports/ad7124_driver_20260921.md)及[近期修复](audit_fixes_20261015.md)。

## 6. 温控边界

PC或B2本地屏幕控制B2，全部加热归B2。B1 SPI只接收镜像常温/高温显示模式，不下发加热，也不转发PC温控指令。

## 7. Boot选择

B1 MCP4725在I2C2、地址0x60，仅写DAC寄存器，不写EEPROM；Boot目标0～1.8V，不允许更高。0506设置/0507查询/0508响应已接，查询表示成功配置缓存，不是RK实际启动介质。DAC码以现有驱动公式和回归为准，旧文档手算值不作为常量。VOUT、实际供电误差和RK启动介质待B1验收。

## 8. LED、槽位与W25信息

- 三组结果灯按mirrorIndex 0/1/2映射两组LPDDR/eMCP；常温双通道闪、高温FAIL通道闪，亮/灭各500ms，实际颜色待板。
- 0106按B2已知模式开始显示，未知灭；0107把失败镜像设FAIL、其余PASS，后续模式通知不覆盖最终结果。
- PG6是MCU正常上电指示；PB3～5为输入、无内部上下拉，原始0～7加1作为siteId。槽位不是镜像编号，也不是B2在位检测。
- 有W25时Boot初始化/迁移FRU，App只读；未焊W25的B1首板显式跳过，PC暂用JMCT-4000 / D00 / RK3588_test宏定义。固件版本及升级状态均不读写。
- 精简Boot只准备FRU、检查App向量并跳转，不含OTA、回滚或安装器；旧下载区、启动日志及RK两块8MiB地址不动。见[Flash布局](flash_layout.md)、[直接烧录](direct_flash.md)。

## 9. 协议与业务

现行定义分别见[PC](pc_tcp_protocol_spec.md)、[RK](protocol_spec.md)、[B2](b1_b2_spi_protocol.md)。三者不共享业务序号。

- PC发送流水号双向独立，不回显；0006为通用执行状态，不是错误专用包。
- FUNC最终结果单包3/5/7 TLV原子解析，两颗mirrorIndex=0/1；siteId及measurementData位于0105顶层。进度为5001+3016同帧，不凭PASS打印文本完成测试。
- 完成结果前保持在途；断线或提前0107不等于RK已结束。无自动重发算法。
- FUNC按电压/Boot变化或`reboot:true`决定冷重启；SLT每次匹配slt/memtest86均重新启动。PARA尚未执行。
- `TimingParameters`只解析存RAM，未绑定RK，不可把参数保存成功解释成硬件已应用。

## 10. 验证入口

| 范围 | 入口 |
|---|---|
| 普通构建/架构 | `tools/dev.ps1 build`、`style-test` |
| PC/RK/B2实际C | `pc-protocol-test`、`protocol-test`、`b2-protocol-test` |
| B1同源码回归 | 对应`tests/verify_*_c.py --app-root ../b1`，以脚本帮助为准 |
| 采样/电源 | `verify_ad7124_c.py`、`verify_pac1934_c.py`、`verify_monitor_boot.py`、`verify_uart_idd_c.py`及B1电源检查 |
| 日常SWD/空白板下载 | [直接烧录](direct_flash.md)，执行前确认空闲；不得当只读检查运行 |
| 第二块SPI主机 | `tools/b2_spi_master/build.ps1`，只构建不自动烧录 |
| 单板双SPI | `../b2_spi_loopback/build.ps1`仅构建；该独立目录Ctrl+Shift+B确认后备份并清空内部Flash下载；实板脚本需确认镜像/接线后运行，见其README |

<a id="board-pinout"></a>

## 11. 引脚总表

这是当前软件绑定速查，不代表本轮重新核图或电气验收。B1详细依据见[引脚映射](b1_mcu_pinout.md)；V2/B1禁止互相照搬端口。

| 功能 | B1 | 阿波罗V2 |
|---|---|---|
| RK协议 | UART4，PA12 TX / PA11 RX，RS0102→RK UART0_M0 | USART1，PA9 TX / PA10 RX |
| RK日志/本地输出 | USART2 PD6接收RK日志；USART1输出所选MCU/RK日志 | USART1仍作协议，不照搬B1日志配置 |
| TPS VDD1 | I2C1 PB8 SCL / PB7 SDA | 以V2总线绑定与模拟配置为准 |
| TPS VDD2/Boot DAC | I2C2 PF1 SCL / PF0 SDA | 不宣称已有真实DAC |
| TPS VDDQ/PAC | PH4 SCL / PH5 SDA，软件开漏I2C | 保留V2接口绑定 |
| AD7124 | SPI4 PE11 CS / PE12 SCK / PE13 MISO / PE14 MOSI | SPI2 PB12～15 |
| B2 SPI | SPI2 PB12 NSS / PB13 SCK / PB14 MISO / PB15 MOSI | SPI3 PA15 NSS / PB3 SCK / PB4 MISO / PB5 MOSI；PB5 AF7，其余AF6 |
| ETH RMII | PA1/PA2/PA7/PB11/PC1/PC4/PC5/PG13/PG14；PG12复位PHY | YT8512C，PCF8574复位路径 |
| 电源 | PC6共享4V / PC7系统3.3V；PG11预留NC | 正式实电源配置未启用，模拟为显式构建选项 |
| 结果灯 | PG0～5三组双色；PG6正常上电 | PD7/PD3/PD2/PC12/PC11/PC10为六路结果输出 |
| 槽位 | PB3/PB4/PB5输入，PB3最低位，原始码加1 | `B2_TEST_SITE_ID`默认1；PB3～5用于SPI不能同时读槽码 |
| SWD | PA13 SWDIO / PA14 SWCLK；J5四孔含GND/3V3_MCU，无NRST | 按开发板SWD接口接线 |

芯片身份、MISO回读、模拟电压和多从机电气条件仍待实板，不以引脚静态检查代替。
