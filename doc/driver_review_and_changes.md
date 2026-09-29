# 驱动核查与修改记录

## 记录规则

本文件集中记录发现的问题、修改原因、涉及文件、实际修改和验证结果。日期、历史经过、修复过程不再写入驱动代码或函数注释。

代码注释只保留使用接口和理解实现必需的内容：用途、参数、返回值、单位、有效范围、时序、并发与硬件约束。不能因为减少历史描述而删掉这些约束。

“待修复”不等于“已经修改”；“离线通过”不等于“真实芯片通过”。后续修复沿用本文的问题编号更新状态，不另造一套问题清单。

## 2026-09-22：软件修复与开发板验证（当前结论）

本节承接下面的审查清单，**DR-01～09、R-01～08的可确定软件项已修复或补齐**。原有审查段保留当时证据，不再代表当前代码仍未修复。V2/B1共同驱动已同步；B1的SPI4、20mΩ、1.01分压、双RK串口选择和SPI2从机职责保持不变，未修改完整B工程。

### 修改对应表

| 问题 | 实际修改与涉及位置 | 软件状态与剩余硬件边界 |
|---|---|---|
| DR-01、R-07 | 两板 `eth_custom_phy_interface.c/.h`联合验证PHYID1/2；读取0x11已解析速率/双工。MDIO读取失败、非法模式和未解析状态拒绝使用；SetLinkState不再失败后继续写。 | 已补齐实际C四模式/故障测试；V2四种模式实测见下表。 |
| DR-01调用链 | 两板 `ethernetif.c`在TCPIP线程串行更新MAC速率/双工和link状态；启停与接收共享互斥，发送BUSY只有限重试；HAL失败保持down并允许下次重试。 | 不增加新的网络任务或协议；软件重新协商通过，不等同于已做物理插拔/EMC。 |
| DR-02 | V2 `Core/Src/spi.c`在USER CODE内固定SPI2 256分频，约504kHz；正式monitor不再绑定PB12为AD片选。测试路径PB12采用开漏而非推挽。 | **PB12实际共用PCF INT，未接独立CS前正式V2 AD禁用**；B1仍使用SPI4/PE11。不能把这个板级限制写成ADC硬件已经验收。 |
| DR-03～05 | 两板 `ad7124.c/.h`开启适用诊断并回读ERROR_EN、配套CS_EN；保留last_status/last_error，读ERROR清锁存，本次异常数据仍拒绝交付；POR使句柄失效。复位逐事务检查、轮询POR结束后等待就绪。 | 不开启未接外部参考检测或会改变帧长的SPI CRC。无真实ADC接入，故障注入是实际C＋SPI模型。 |
| DR-04组合层 | 两板 `memory_monitor.c`在ADC失信后下次重初始化；AD初始化失败不再阻断PAC独立初始化。 | 不伪造采样值，不因AD失败重置其它电源/器件。 |
| R-02～04、R-08 | AD每个SPI事务后保证至少1ms间隔；校准上限覆盖低速长校准；三通道注释按滤波建立说明，FS384约240ms一轮而非每路50SPS；单极性分母改为2^24；初始化禁用全部未用逻辑通道3～15。 | 间隔保证依赖已明确的wait_ms/HAL时基；14秒模型校准通过。实际带宽、波形与精度待模块/B1验收。 |
| DR-06、DR-09、R-01、R-05 | 两板 `qspi_hw.c/.h`取消初始化66/99复位，检查BUSY/SUS、完整JEDEC、ADS/QE；WREN回读WEL；程序页回读比较、擦除逐段确认全FF；0xEB显式MODE=0xFF＋4 dummy；整片擦除等待410s。 | 现有OTA/元数据事务层校验保留；整片擦除只在虚拟时钟测试，实板**没有整片擦除**。 |
| DR-07、R-06 | V2 `pcf8574.c/.h`改为ACK成功才更新输出影子，P1/P5输入始终写1释放；NACK使影子失效，不再读取引脚电平回写整个锁存。软件I2C按时钟留至少5µs阶段延时。PHY复位调用方检查返回值。 | 板上初始化/复位链路实测通过；NACK/被外部拉低输入由实际C模型验证，未用分析仪测量I2C高低电平。 |
| DR-08 | 两板 `mcp4725.c/.h`按VDD×code/4096换算、最大码4095饱和；更新Boot测试预期1500mV/3300mV=1862。 | 0～1.8V的Boot限制不变，真实输出校准待B1。 |

TPS和PAC前一轮已修复项本次回归通过，未扩展IDD业务、不改变约2秒后台采样节奏，不增加PC/RK JSON字段。默认OFF、未配置时序门禁、RK未确认接口的拒绝执行均保留。

### 可复跑的软件验证

| 项目 | 结果 | 边界 |
|---|---|---|
| `verify_ad7124_c.py`，V2/B1 | PASS | 实际AD、监测C；含复位/每步配置失败、ERROR读清、POR与ERROR同时出现、14秒校准、输出不被半份数据覆盖。 |
| `verify_pac1934_c.py`，V2/B1 | PASS | 实际PAC和等待/组合层C；V2 10mΩ、B1 20mΩ差异保留。 |
| `verify_monitor_boot.py`，V2/B1 | PASS | 正式monitor任务、IDD统计、Boot缓存；检查两板不同ADC绑定。 |
| 新增 `verify_driver_reliability.py`，V2/B1 | PASS | 实际NOR/MCP/PHY和生产MAC更新函数；PCF只引用V2实现。覆盖WEL拒绝、坏回读、长擦除/时钟回绕、HAL/MDIO故障、DAC边界、PCF输入及NACK。 |
| `verify_ota_app.py`，V2/B1 0.1.8包 | 四组均PASS | 实际OTA C；正常提交、部分写失败、坏包、日志已写但返回错误。 |
| Debug：V2/B1 | PASS | App分别229584/221372字节；仅构建，非普通布局镜像烧录。 |
| V2 driver-mock镜像 | 构建PASS | 未绕过OTA部署此普通布局Mock镜像；不声称本轮跑过板上Mock。 |
| OTA槽限Release：V2、B1 UART4、B1 UART2 | A1/A2双向逐字节一致 | 0.1.8 V2 App 143696字节、1359处修正、包149248字节；B1 App 138980字节、1346处修正，UART4包144480字节。 |
| V2 PC、RK、接口边界、Boot模型；B1项目检查 | PASS | PC 24项、RK 96项、边界19项及纯接口头、Boot 7项、B1 46个引脚与合同检查。 |

复跑示例（脚本在V2工程；B1通过参数选取源文件）：

```powershell
$gcc = 'C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe'
python -X utf8 tests/verify_driver_reliability.py --compiler $gcc
python -X utf8 tests/verify_driver_reliability.py --app-root D:/pro/b1 --compiler $gcc
python -X utf8 tests/verify_ad7124_c.py --compiler $gcc
python -X utf8 tests/verify_ad7124_c.py --app-root D:/pro/b1 --compiler $gcc
```

### V2真实开发板验证

经用户授权使用已连接开发板；先备份内部Flash全部2MiB及W25本轮相关范围0～0x40FFFF（后者读取两次一致），再沿**现有TCP OTA链路**部署，没有覆盖完整B或使用旧最小Boot。

- 0.1.5 → 0.1.7：新驱动安装到A2，30秒健康确认、0903结果、心跳正常。
- 在0.1.7新驱动上实测：字段/Base64错误、3001字节块拒绝、4096整帧半包/粘包、重复块、取消、维护门、断线重启下载、30秒空闲取消、整包CRC错误、4097字节PDU断连及重连。
- 0.1.7 → 0.1.8：通过新驱动实际擦写W25，完整包提交、重复finish幂等、提交后cancel拒绝；完整B安装到A1，30秒确认、0903和心跳正常。两版仅验证版本号不同，驱动代码一致。
- 内部Flash读取比对：B区128KiB与备份一致；A1与0.1.8编译产物逐字节一致，A2与0.1.7的地址修正产物逐字节一致。
- 实际YT PHY ID=0x0000/0x0128。仅临时改变本板能力广播，未修改PC网卡；各模式均重新协商后检查MAC并通过TCP版本查询。原ANAR=0x1DE1，测试后恢复全部原能力。

| 实际协商 | PHY 0x11 | MACCR | TCP查询 |
|---|---|---|---|
| 10M全双工 | 0x3F80 | 0x3830A003 | PASS |
| 10M半双工 | 0x1F80 | 0x38308003 | PASS |
| 100M半双工 | 0x5F80 | 0x3830C003 | PASS |
| 100M全双工 | 0x7F80 | 0x3830E003 | PASS |

原始证据保存在 `build/driver-hw-20260922/`：`backup.json`、`upgrade-017-retry1.json`、`exercise-upgrade-018.json`、`network-*.json`、`phy-*.log`、内部Flash回读。首次复位后过早连接的超时报告 `upgrade-017.json`保留，不隐藏失败尝试。

W25最终回查：FRU两个扇区、0x6000～0xFFFF保留元数据和完整2MiB稳定版区均与备份逐字节一致；下载区与0.1.8包逐字节一致。FRU SHA-256仍为 `2aa4dd421dc9e905f737d5f648ecd374dbd0ae9a8e7f3ca88716a99078d2d41c`。证据为 `final-w25-retry1.bin`、`final-check.json`。首次只读导出因两个调试会话重叠而中止，未写Flash；改成串行后完整重读通过，失败日志 `final-w25.log`保留。

收尾已从RAM读取工具复位回正式App：A1 0.1.8完成健康确认，测试仪信息查询、连续50次心跳通过；0504返回off，0602返回off/validMask=0且不伪造电压/电流字段。PHY最后回读ANAR=0x1DE1、BMCR=0x1140，与原配置相同，100M全双工。无外接TPS/PAC/AD/MCP和B1真板，不把离线通过或开发板其它器件通过外推为这些硬件已验收。未开启RK电源或加热；未提交、未推送。

## 2026-09-22：芯片手册对照审查（修复前快照）

### 本次实际修改

- 新增本记录；新增临时、离线的实际 C 驱动复现程序。
- **没有修改生产驱动、业务代码或原有注释；没有连接、复位、烧录开发板；没有提交或推送。**
- 工作区在审查前已有 AD/PAC、监测层和串口等未提交修改。本次按这些修改后的现状核查，没有撤销或冒称为本次实现。
- 快照基线：V2 `d050df6`、B1 `42e431b`、Boot `f5d3d62`，均须结合当时工作区改动理解，不能只凭提交号复现所有结论。

### 范围与依据

范围是项目自有的器件驱动、总线适配和直接调用路径：TPS62868/69、PAC1934、AD7124-4、MCP4725、W25Q256、YT8512C/H、V2 PCF8574，以及 SPI/I2C/QSPI、等待时基和看门狗配置。不是重新认证整份 STM32 HAL、FreeRTOS、LwIP，也没有重做整板硬件评审。

| 器件 | 使用的原厂资料 | 本次重点 |
|---|---|---|
| TPS62868/69 | [TI SLVSFS3B](https://www.ti.com/lit/ds/symlink/tps62868.pdf)，8.2、8.5、8.6 节 | VSET/地址、型号倍率、VOUT 编码、CONTROL/STATUS、复位条件 |
| PAC1934 | [Microchip DS20005850E](https://ww1.microchip.com/downloads/en/DeviceDoc/PAC1931-Family-Data-Sheet-DS20005850E.pdf)，启动、REFRESH、寄存器章节 | 通道关闭位、ACT/LAT、VPOWER、单位换算、转换等待 |
| AD7124-4 | [ADI Rev.E](https://www.analog.com/media/en/technical-documentation/data-sheets/ad7124-4.pdf)，表 4、52、66、71、72，复位/校准/滤波章节；[ADI no-OS 实现](https://github.com/analogdevicesinc/no-OS/blob/main/drivers/adc/ad7124/ad7124.c) | SPI 时序、复位、诊断、采样完整性、通道/基准、校准 |
| MCP4725 | [Microchip DS20002039E](https://ww1.microchip.com/downloads/aemDocuments/documents/MSLD/ProductDocuments/DataSheets/MCP4725-Data-Sheet-20002039E.pdf)，公式 5-1、Fast Write 章节 | DAC 传输函数、码值、命令格式、电压范围 |
| W25Q256 | [板卡资料内 Winbond W25Q256FV Rev.G](<E:/【正点原子】阿波罗V2 STM32H743开发板/7，硬件资料/7，硬件资料/2，芯片资料/W25Q256.pdf>)，8.2.20、8.2.53、AC Characteristics | 4 字节地址、页/扇区边界、WEL/BUSY/SUS、复位、回读、读模式 |
| YT8512C/H | Motorcomm《YT8512C YT8512H Datasheet》，本地文件 `D:/pro/test/tmp/schematic_review_20260920/YT8512H.pdf`，Clause 22 寄存器 0/1/2/3/4/5/0x11 | PHY 探测、协商结果、MAC 配置是否匹配 |
| PCF8574 | [板卡资料内 Philips PCF8574](<E:/【正点原子】阿波罗V2 STM32H743开发板/7，硬件资料/7，硬件资料/2，芯片资料/PCF8574.pdf>)，7.3、I2C 时序 | 准双向口、输入/输出锁存、ACK、100kHz 上限 |

注意：本地名为 `AD7124中文手册.pdf` 的文件首页实际写的是 **AD7124-8**，没有用它替代本项目 AD7124-4 的型号/引脚依据。W25 的 `EF4019` 不能区分 FV/JV，不能据此擅自采用仅某一后缀支持的命令；本次 Flash 兼容性按现有 Boot 的 FV 基线检查。

### 结论概览

优先级“高”表示建议进入下一轮软件修复，不表示已实测发生故障；“普通”表示误差或已被上层部分兜底的可靠性问题。

| 编号 | 问题 | 范围 | 优先级 | 状态/证据 |
|---|---|---|---|---|
| DR-01 | PHY 链路速率/双工结果没有同步给 MAC | V2、B1 | 高 | 代码链路确认；待修复 |
| DR-02 | 正式 V2 镜像的 AD 片选/时钟配置与测试镜像不一致 | V2；不套用到 B1 | 高 | GPIO/时钟/调用路径确认；待修复 |
| DR-03 | AD 只检查 ERROR_FLAG，却未启用主要测量诊断 | V2、B1 | 高 | 手册、实际 C 初始化复现；待补齐 |
| DR-04 | AD 错误锁存及意外复位缺少正式恢复路径 | V2、B1 | 高 | 实际 C 复现、调用链确认；待修复 |
| DR-05 | AD 复位没有可靠确认“已可配置” | V2、B1 | 高 | 源码、原厂实现对照；待修复 |
| DR-06 | App Flash 初始化未判 BUSY/SUS 即软件复位 | V2、B1 App | 高，条件触发 | 实际 C 总线模型复现；完整 B 正常启动路径已有保护 |
| DR-07 | PCF 单脚读改写会改变其他输入的输出锁存；ACK 被忽略 | 仅 V2 | 普通 | 实际 C 复现、源码确认；待修复 |
| DR-08 | MCP4725 换算使用 4095，而非 4096 | V2、B1 | 普通 | 手册公式、实际 C 复现；待修复 |
| DR-09 | Flash 底层“等到不忙”不能证明写擦已完成 | V2、B1 App | 普通/加固 | 实际 C 复现；OTA/信息记录上层已有回读兜底 |

### DR-01：MAC 没有跟随 PHY 的实际链路模式

位置：[V2 ethernetif.c:644](D:/pro/test/LWIP/Target/ethernetif.c:644)、[B1 ethernetif.c:628](D:/pro/b1/LWIP/Target/ethernetif.c:628)、[PHY 状态读取](D:/pro/test/LWIP/Target/eth_custom_phy_interface.c:223)。

`USER_PHY_GetLinkState()`区分 10/100M、半/全双工，但链路任务只调用 `netif_set_link_up/down()`。项目自有代码没有依据结果调用 `HAL_ETH_SetMACConfig()`；当前 ST HAL 初始化默认是 100M 全双工（`stm32h7xx_hal_eth.c:3006、3027`）。

因此，常见的 100M 全双工环境可以通信，并不能证明 10M 或半双工环境正确；换到这些环境可能显示 Link Up，却通信异常。

修复方向：有效链路状态变化时，按 HAL 要求同步 MAC 的速率、双工和启停顺序。YT 的实际结果可由 0x11 寄存器读取；不能只用双方广播能力交集代替所有情况下的实际链路结果。错误/未解析完成状态不得直接标为链路正常。

验收：100M 全双工、10M、半双工及反复插拔分别测试。未宣称本轮做过这些实板测试。

### DR-02：V2 的 AD 测试初始化没有进入正式监测路径

位置：[GPIO](D:/pro/test/Core/Src/gpio.c:106)、[SPI2 配置](D:/pro/test/Core/Src/spi.c:40)、[正式绑定](D:/pro/test/App/tasks/common/monitor_task.c:289)、[测试初始化](D:/pro/test/App/tasks/test/test_task.c:123)。

- 正式监测层把 `PB12` 当作 AD 的 CS，但普通 GPIO 初始化及 PCF 初始化将它配置为输入；普通镜像默认不开器件自检。
- SPI2 普通配置为 PLL2、2 分频。由 HSI=64MHz、M=32、N=129、P=2 推算内核约 129MHz，SCLK 约 **64.5MHz**。ADI 表 4 要求 SCLK 高、低各至少 100ns，即不超过约 **5MHz**。
- 自检/波形任务另外改为输出 CS、低速 SPI。自检完成还会恢复旧的高速配置。因此“测试波形通过”不能替代普通监测镜像的接入验证。

B1 是另外的情况：使用 SPI4、PE11 片选、Mode 3、256 分频（约 390.6kHz），不能把 V2 的 SPI2 配置直接同步过去。

修复方向：把 V2 实际使用 AD 时所需的片选/速率放到正式板级初始化；先处理 PB12 与 PCF INT 的板上占用关系，不能只把 PB12 强改为推挽输出。若 V2 普通镜像不接真实 AD，则应明确禁用该硬件绑定。

### DR-03：AD 的主要诊断仍处于默认关闭状态

位置：[初始化](D:/pro/test/BSP/AD7124/ad7124.c:255)、[状态检查](D:/pro/test/BSP/AD7124/ad7124.c:78)。

ERROR_EN 复位值为 `0x000040`，只启用 SPI_IGNORE 检查。初始化没有写 ERROR_EN；因此仅有 ERROR_FLAG 判断，并不等于已经具备饱和、转换异常、校准异常等检测。

实际 C 复现中，初始化后 ERROR_EN 仍为 `0x40`，没有错误标志的满码数据被监测层标为有效。这个复现证明“软件未开启/检查相应诊断”，不声称所有满码必然是物理故障，也不能据此把 0V 一律判错。

修复方向：按实际接线选择诊断位，并把异常带入采样质量状态。不要直接全开所有位：内部基准不对应外部参考检测；SPI CRC 会改变帧长度；部分 SPI 检查要求 CS_EN 配套。

### DR-04：AD 故障发现后没有恢复闭环

位置：[wait_ready](D:/pro/test/BSP/AD7124/ad7124.c:78)、[ERROR 读取接口](D:/pro/test/BSP/AD7124/ad7124.c:474)、[监测组合层](D:/pro/test/HAL_Drv/memory_monitor.c:23)。

ERROR_FLAG 出现时，驱动会拒绝样本，这一点正确。但正式路径没有调用 `ad7124_read_error()`保存/清除诊断；`memory_monitor_sample()`失败后也没有使 `adc_ready` 失效。因此后续 `memory_monitor_init()`仍跳过 AD 初始化。

离线注入一个锁存错误后，连续三个“采样→初始化”周期都无法恢复：错误寄存器读取次数为零、复位次数不增加、`adc_ready`仍为 1。结果是持续没有有效电压数据，不是悄悄上报旧值。

另外，STATUS 中的 POR_FLAG 被读取后清除，但当前采样路径不处理这个事件。芯片意外复位后寄存器配置已经丢失，软件不能继续认为原配置有效。

修复方向：保留故障原因，失效相应器件状态，有界地重新初始化；不要为了清错误而丢掉诊断信息，也不要把一次临时忙当成必须复位整板。

### DR-05：AD 复位成功的判断不充分

位置：[ad7124_reset](D:/pro/test/BSP/AD7124/ad7124.c:130)。

当前流程是 64 个“1”→等待 1ms→尝试读取 STATUS；但 STATUS 读取失败也不影响返回值，POR/可写状态没有实际用于判定。上层随后马上写配置，仅凭 SPI 传输成功不能确认 ADC 已接受这些写入。

原厂手册明确复位需要完成内部过程；ADI no-OS 当前实现先等待电源复位状态，再留 4ms 配置恢复裕量。现有 `device_wait_ms(1)`保证的是至少 1ms，不是“芯片必然已可配置”。

修复方向：有界确认复位完成，满足配置恢复时间并检查关键配置/诊断；不要单纯把注释改成“已确认复位成功”。本轮没有实测现有板上的初始化失败概率。

### DR-06：App 在 Flash 忙时直接复位的风险

位置：[App qspi_init](D:/pro/test/HAL_Drv/qspi_hw.c:43)、[完整 B 的保护](D:/pro/bootloader/boot/boot_port.c:121)。

App 的 `qspi_init()`直接发送 `66h+99h`，此前不检查 BUSY/SUS。Winbond 8.2.53 明确警告：在擦写进行中或挂起时复位，可能损坏该次操作的数据。MCU 复位不一定让外部 Flash 同时掉电。

总线模型设定 BUSY=1 后，实际 App 驱动仍发送了复位，复现的是不安全命令顺序；没有在实板上故意中断擦写。

边界：完整 OTA B 已先等 NOR 空闲并拒绝 SUSPEND，正常由该 B 启动 App 的路径因此得到保护。主要残余风险在旧最小 Boot、直接调试 App 或运行中重新初始化。不能把这一项说成“现有 OTA 必然损坏 Flash”。

修复方向：App 初始化也应先确认空闲/非挂起，尽量不做不必要的 NOR 复位；不擅自清保护位。

### DR-07：PCF8574 的单脚操作不是普通 GPIO 的读改写

位置：[pcf8574_write_bit](D:/pro/test/BSP/PCF8574/pcf8574.c:83)、[read/write_byte](D:/pro/test/BSP/PCF8574/pcf8574.c:49)。

PCF 输入必须在输出锁存器中保留 1。当前先读实际电平、改一位、再写整字节。如果输入 P1 被外部暂时拉低，操作 PHY 复位 P7 时会把 P1 的 0 一起写回；外部释放后，P1 仍被自己的输出锁存器拉低。

实际 C 复现：锁存初值 FF、外部 P1=0，写 P7=0 后锁存变为 `0x7D`；外部释放 P1 后读值仍为 0。

此外，读写忽略地址/数据 ACK；芯片无响应时读回全 1 有可能被当作复位脚已拉高。当前使用点主要是 V2 PHY 复位，不影响不使用 PCF 的 B1。

修复方向：保留输出影子值并让输入位始终写 1；错误必须传给调用方。这里不需要引入复杂 GPIO 框架。

### DR-08：MCP4725 的 4095/4096 换算错误

位置：[mcp4725_mv_to_code](D:/pro/test/BSP/MCP4725/mcp4725.c:25)、[现有测试](D:/pro/test/tests/test_boot_mode.py:17)。

手册公式是 `VOUT = VDD × code / 4096`，不是 `/4095`。12 位最大码是 4095，与传输函数分母是 4096 并不矛盾。

例如 VDD=3300mV、目标=1500mV：当前返回 1861，按手册四舍五入应为 1862，差一个码（约 0.806mV）。这是小误差，不应夸大成 Boot 模式全部错误。

现有 Python 测试也采用 4095，并预设 1861，所以它通过不能证明公式正确。

修复方向：按 4096 换算，处理最大码饱和边界，更新独立预期值；Boot 电压不得超过 1.8V 的约束保持不变。

### DR-09：Flash 底层成功返回的含义过宽

位置：[写使能](D:/pro/test/HAL_Drv/qspi_hw.c:31)、[擦除](D:/pro/test/HAL_Drv/qspi_hw.c:182)、[页写](D:/pro/test/HAL_Drv/qspi_hw.c:262)。

底层发送 WREN 后不验证 WEL，写擦后只等 BUSY=0。受保护或被忽略的操作也可能表现为“不忙”；HAL 发送成功只是总线传输成功。

模型让芯片拒绝写擦、数据保持 `0x5A`，实际 `qspi_write_page()`和`qspi_erase_sector()`均返回成功。

这是底层可靠性加固项，而非所有业务的实际误报：

- `App/ota/ota_app.c:258`对下载块回读比较；
- `BSP/Flash/flash_layout.c:433`对版本/FRU记录回读验证；
- `App/ota/boot_record.c`对日志回读；
- 完整 B 既验证 WEL，也验证写入内容/擦除全 FF。

修复方向：在最合适的一层保证“业务成功=内容正确”；不能只加 WEL 检查便声称覆盖了所有保护拒绝，也没必要上下三层机械重复整块回读。

### 继续确认或低优先级项

以下不混入“已证明当前实板故障”的清单。

| 编号 | 事项 | 当前判断与下一步 |
|---|---|---|
| R-01 | App `0xEB` 读命令把 MODE 的 2 个周期合并进 6 个 Dummy，未显式输出模式字节 | Winbond 的 MODE 会影响下一笔是否需要指令。当前 `AlternateByteMode=NONE`，不等于保证送出安全模式值；V2 有 GPIO 上拉，B1 GPIO 无内部上拉，不能只凭 V2 读通外推 B1。建议显式 MODE+4 Dummy，但本轮未捕获 B1 波形，不断言已经误进连续读。 |
| R-02 | AD 连续事务间隔 | 表 4/图 6 有 t12 限制。代码未显式保证；现有 HAL 调用开销是否足够，要以实际优化等级及逻辑分析仪确认，不能只看 SCLK 频率。 |
| R-03 | AD 三通道速率被“50Hz”注释简化 | FS=384 的单通道稳态 ODR 为 50SPS；多通道切换须经历滤波建立，三路不是各 50SPS。用于 IDD 联动时要按真实转换窗口定义，不把轮询次数当采样率。 |
| R-04 | AD 校准固定 1000 次轮询 | API 允许低速滤波和增益>1，但内部满量程校准可能需要数秒，固定轮询上限不覆盖所有有效组合。当前默认增益1/FS384不能用来验证所有公开 API 配置。 |
| R-05 | Flash 整片擦除等待上限 | FV Rev.G 的 tCE 最大400s，当前近似200s。此危险 API 不在常规升级路径，本轮未调用；以后保留该接口则按实际型号最大值处理。 |
| R-06 | PCF 软件 I2C 时序 | PCF8574 标准上限100kHz；代码注释声称约250kHz但实际是空循环延时，不能凭注释确定实测速率。需测量并保证100kHz及高低电平/建立保持时间。 |
| R-07 | PHY 探测和失败路径 | 只用 PHYID1 不等于FFFF探测较弱；YT 的 PHYID1 本来可以为0，不能简单把0判无效。应联合型号ID验证。未使用的 SetLinkState 接口还需避免读取失败后继续写。 |
| R-08 | AD 单极性换算分母 | 代码使用2^24−1，应统一核对理想传输函数及截断/舍入。量级小于一个24位码，不作为当前20mV调压的主要误差来源。 |

### 已核对且应保留的实现

| 器件/层 | 本轮未发现新的确定问题的部分 | 仍不等于实板验收 |
|---|---|---|
| TPS62868/69 | 三轨地址0x49/0x41/0x4F、倍率2/1/1、VOUT基码与步长、CONTROL掩码/缓存、STATUS定义 | 真实贴装、电压精度、带载响应、天然启动窗口及RK训练仍待板级验证；I2C ACK不是电压已稳定 |
| PAC1934 | 通道关闭位映射、4字节VPOWER去掉低4位、BE字节序、64位单位换算、REFRESH命令、ACT/LAT检查、失败不交付部分结果、配置失效后的重试 | SLOW脚对实际采样速率的影响、分流电阻误差、小电流零偏、真实IDD窗口/峰值带宽仍须真板 |
| PAC板型差异 | V2为10000µΩ，B1为20000µΩ；没有错误地强制两板相同 | 电阻贴装与实测仍须确认 |
| AD基础配置 | Mode3、内部2.5V基准、AIN0/1、2/3、4/5、负端接地时关闭负输入缓冲、完整三路快照才交付 | 诊断和恢复问题见DR-03～05；不等于ADC全部完成 |
| AD已有校准修正 | 增益1不做不支持的内部满量程校准；其他增益在低/中功耗先满量程后零位；失败尽力恢复 | 超慢组合超时范围见R-04 |
| MCP传输 | Fast Write两字节格式、正常/掉电位、0..4095码值范围、避免频繁写EEPROM、Boot上限1.8V | 换算公式见DR-08；真实VDD和负载误差需校准 |
| 完整B的NOR端口 | BUSY/SUS门禁、WEL确认、四字节模式确认、FV兼容指令、分块页写/扇区擦除和回读 | 没有以离线审查替代掉电注入和长时间升级实测 |
| 等待时基 | `device_wait_ms()`复用现有TIM1/HAL毫秒时基，补偿tick相位并让出CPU | 不必为PAC的“至少1ms”再开硬件定时器；它不是精准采样触发器 |
| 看门狗 | App/Boot刷新接口与各自分频配置未见新的寄存器级错误 | 超时时间受LSI误差影响；本轮不重新认证系统级健康判定 |

TPS和PAC的上述判断是这次对手册和现有代码的核查结果，不是“保证没有任何潜在缺陷”。B1的SPI2仍归B2保活，温控仍归PC–B2，本次没有改动职责边界。

### 实际执行的验证

| 验证 | V2 | B1 | 性质 |
|---|---|---|---|
| `verify_ad7124_c.py --app-root ...` | PASS | PASS | 编译并在QEMU运行实际AD/监测C代码；不是ADC硬件仿真 |
| `verify_pac1934_c.py --app-root ...` | PASS | PASS | 实际PAC C代码+总线模型 |
| `verify_monitor_boot.py --app-root ...` | PASS | PASS | 实际监测组合层的软件测试 |
| 本次六项缺陷现象复现 | 全部重现 | 对应实际驱动也重现 | B1对照中的PCF用例仍引用V2文件，不宣称B1有PCF |
| `verify_lpddr_power.py` | PASS | 未独立运行 | Python模型/源码合同；输出明确说明未执行新增C Mock |
| `verify_pwr_ctrl.py` | PASS | 未独立运行 | Python模型/源码合同，不是实板时序 |
| `test_boot_mode.py` | PASS，但预期公式有误 | 未独立运行 | 展示测试盲点，不能据此关闭DR-08 |

B1没有上述最后三份同名Python脚本，尝试定位/运行时不存在；没有把它们写成B1通过。B1实际C验证由V2侧脚本的`--app-root D:/pro/b1`完成。

临时复现入口：`D:/pro/test/tmp/driver_datasheet_audit_20260922/reproduce.ps1`及同目录`reproduce.c`。用PowerShell 7直接执行：

```powershell
& D:/pro/test/tmp/driver_datasheet_audit_20260922/reproduce.ps1
& D:/pro/test/tmp/driver_datasheet_audit_20260922/reproduce.ps1 -AppRoot D:/pro/b1
```

复现程序直接链接所选工程的驱动，仅替换GPIO/总线底层；六个“REPRO”断言确认当前缺陷现象存在，**不是修复后应保持不变的正确性测试**。修复时应把相应用例转为期望正确行为的正式回归测试。它不模拟模拟电路精度，也不证明实板故障概率。

### 建议下一步顺序

1. AD诊断、复位及恢复闭环；若接V2 AD模块，一并修正正式片选/速率配置。
2. PHY实际链路模式与MAC同步。
3. Flash初始化安全及读模式；保留现有OTA/元数据回读保护。
4. MCP换算及V2 PCF小范围修正。
5. 分别做软件回归，再按接入的真实器件做示波/逻辑分析仪和故障注入验收。

以上均为待执行项。本轮停留在审查和记录，没有提前实现。
