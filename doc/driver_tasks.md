# MCU 当前进度与验收清单

更新：2026-10-21。本页只维护当前状态；已完成过程、旧方案和历史测试数字按需从主题记录读取。[精简前原文](history/cleanup_20261020/README.md)可完整恢复。

<a id="current-status"></a>

## 当前进度与下一步

| 模块 | 当前代码状态 | 已有验证与剩余边界 |
|---|---|---|
| PC TCP | 18字节头+cJSON、状态查表和命令查表；半包/粘包、异常输入恢复、独立发送Seq | V2既有实板TCP验证；最新`TimingParameters`接收存储已随0.1.38安装，解析离线通过，尚未下发RK或实测其生效 |
| 整轮/FUNC/SLT | 0106开始整轮；0103/0105功能测试；010A/010B整次SLT；0107失败数组收尾；070x删除 | 两板实际C回归；V2已有整轮及网络验收。真实SLT服务器下载待系统联调，0108/0109 PARA仍预留 |
| RK链路 | 独立任务、1秒心跳；双TLV进度；单包3/5/7 TLV双颗粒结果；在途事务和Seq窗口 | V2真实RK链路已有阶段记录，实际C回归通过；B1 UART4/RS0102电气验收待板，不宣称所有算法/负载已验收 |
| B2链路 | 独立B2任务/协议/CS状态机，SPI DMA，0004状态交换、动态IP及镜像模式显示；已同步双SPI实测后的DMA错误分类和诊断 | 两板实际C/PC整轮/busy、构建及OTA双槽回归通过；2026-10-21已安装V2 0.1.38，SPI3及接收队列初始化确认。正式镜像本轮没有SPI主机请求；B1八槽、电气与负载仍待验收，见[恢复安装记录§9](b2_spi_integration_20261020.md#9-v2恢复与0138安装2026-10-21) |
| 第二块V2主机 | `tools/b2_spi_master`裸机USART1+SPI3，只有query/id/mode | 已构建、编解码离线通过；未烧录/互联，不包含OTA、网络或采样 |
| 单板双SPI联调 | 同级`../b2_spi_loopback`完整独立Flash工程；SPI2主机→SPI3从机，真实lwIP、板载灯及协议日志；测试主机/串口CLI不加入B1/V2正式构建 | 查询、坏CRC恢复、重复Seq应答已有实板日志；2026-10-21用户确认双SPI及实际IP切换成功。DMA修正已同步正式两板；灯态、跨板时钟/复位和八槽仍分别验收，不能由换IP成功推定 |
| B1电源/Boot | 正式0103与050x接真实驱动、可调初值、配置变化冷重启、本轮普通心跳就绪和直接关电 | 实际C故障/边界回归及构建通过；天然训练窗口、供电/放电时序、DAC输出待B1 |
| TPS/PAC/AD7124 | 三轨调压、真实采样、错误传播及恢复已接监测层；B1使用20mΩ分流 | 实际C/Mock及部分V2发送波形通过；没有真实器件精度或B1上电验收 |
| 采样统计 | 0602最近快照/统计；0105顶层共享本次测试的均值、样点最大/最小值 | 两板软件回归；V2模拟值不是实测。IDD条件、测量窗口/限值仍待RK与器件资料确定 |
| LED/槽位/日志 | PG6初始化后常亮；PB3～5读取0..7加1；三组结果灯；B1本地切换MCU/RK日志 | 引脚检查/构建通过；真实颜色、槽位贴装、串口波特率与连续日志负载待B1 |
| W25/OTA | W25版本/FRU双副本；完整B安装、A1/A2地址修正、健康确认与回滚；网络升级接口已接 | V2已有Flash及网络OTA实测；2026-10-21日常SWD完成0.1.37/A1→0.1.38/A2安装、回读与健康确认。B1日常SWD及空白板首烧仍待实板验收 |

### 软件与板上版本分开记录

- 2026-10-21已从独立双SPI镜像恢复完整V2，并通过日常SWD升级到`0.1.38/A2/confirmed`；TCP实查包标识`79C8CF12`、逻辑电源OFF，地址仍为`192.168.1.201:8888`。恢复与验证见[接入记录§9](b2_spi_integration_20261020.md#9-v2恢复与0138安装2026-10-21)。
- 真正的恢复源是`../b2_spi_loopback/build/flash-20261021-110516-712531/internal-before.bin`，实查与W25的`0.1.37/A1`匹配；11:36目录中的备份已经是独立测试程序，不能用它恢复正式版本。本轮替换前的测试镜像备份在`build/v2-restore-20261021-161844-527904/`。
- 最新时序字段与B2接入已在V2安装，不等于对应业务全部完成实板验收；B1只同步源码，未烧录。离线版本0.1.0仍不是安装版本。
- V2日常任务保留电源/测量模拟和MATS第一颗假FAIL；B1使用真实驱动、不带假FAIL。必须按构建及安装记录判断，不能从没有`simulation`字段推断为实测。

### 下一步

1. V2已恢复正式Boot+App并安装0.1.38；下一步用[两板主机](../tools/b2_spi_master/README.md)在正式TCP/RK/采样任务同时运行时复测B2，验收独立时钟/复位及总线电气。原单板回环的SPI2主机和串口CLI没有带入正式镜像。
2. 验收PC已连接、测试/电源/OTA在途时改IP的busy，断开后以新Seq重试；检查ARP、新IP连接、MISO未选中高阻及DMA/任务余量。
3. V2日常ST-Link正常安装已实测；继续验收空白板Boot+App首烧和B1日常下载，异常断电/回滚按专门测试计划进行，必须事先确认空闲、备份并区分入口。
4. B1到板后按原理图逐项验收：独立MCU供电/默认OFF → 总线和芯片ID → 电压/电流标定 → Boot/训练窗口 → RK链路 → B2八槽。
5. 等RK确认`TimingParameters`下发、IDD算法/保持条件/测量窗口和RK固件升级方式；不能把已存参数当作已生效。
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

B1 SPI4 PE11～14、Mode3；V2临时SPI2。按AD7124-4和ID白名单0x04/0x06/0x07实现，内部2.5V基准、AIN0/1、2/3、4/5差分对。驱动错误传播、校准失败失效和完整快照已有实际C检查；具体封装/BOM、真实ID、输入配置与标定仍须真板确认。见[AD回归](test_reports/ad7124_driver_20260921.md)及[近期修复](audit_fixes_20261015.md)。

## 6. 温控边界

PC或B2本地屏幕控制B2，全部加热归B2。B1 SPI只接收镜像常温/高温显示模式，不下发加热，也不转发PC温控指令。

## 7. Boot选择

B1 MCP4725在I2C2、地址0x60，仅写DAC寄存器，不写EEPROM；Boot目标0～1.8V，不允许更高。0506设置/0507查询/0508响应已接，查询表示成功配置缓存，不是RK实际启动介质。DAC码以现有驱动公式和回归为准，旧文档手算值不作为常量。VOUT、实际供电误差和RK启动介质待B1验收。

## 8. LED、槽位与W25信息

- 三组结果灯按mirrorIndex 0/1/2映射两组LPDDR/eMCP；常温双通道闪、高温FAIL通道闪，亮/灭各500ms，实际颜色待板。
- 0106按B2已知模式开始显示，未知灭；0107把失败镜像设FAIL、其余PASS，后续模式通知不覆盖最终结果。
- PG6是MCU正常上电指示；PB3～5为输入、无内部上下拉，原始0～7加1作为siteId。槽位不是镜像编号，也不是B2在位检测。
- W25版本/FRU运行时加载后组PC响应，不能直接从编译常量应答。当前测试仪默认信息为JMCT-4000 / D00 / RK3588_test。
- 完整B/A1/A2、稳定恢复包、双副本记录和升级工具均已有实现；不再使用“OTA仅预留”的旧结论。分区与恢复规则见[Flash布局](flash_layout.md)、[OTA设计](ota_design.md)。

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
| 日常SWD/网络下载 | [下载说明](ota_daily_download.md)，执行前确认空闲；不得当只读检查运行 |
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
