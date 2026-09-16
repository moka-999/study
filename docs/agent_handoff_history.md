# 交接历史摘录

2026-09-08从整理前的工作区文件归档，保留当时原文与验证数字，不代表当前状态或持续有效的执行指令。下文“当前/本轮/尚未/待接入”仅指各记录日期；当前进度和待办以 [驱动任务清单](../doc/driver_tasks.md#current-status) 为准。原理图摘录保留历史依据，使用时仍须对照任务清单的硬件约束与实际原图。

## 原AGENTS第六节（原文）

## 六、当前进度

- ✅ 2026-09-02：消除0x0006“错误应答”歧义：线命令仍为`CMD_COMMAND_RESP(0x0006)`，语义统一为通用命令执行结果；JSON字段由`commandId`改为`requestCommandId`，`status=accepted|completed|failed`，仅failed携带`errorCode`。V2/B1代码、离线合同、Markdown和Word已同步；V2 PC离线24/24、B1完整检查、两板正式/网络/Mock ARM构建通过。准备重烧联调板时ST-Link、CH340和`.201`同时掉线，因此最新字段尚未重新烧入；磁盘中的原App备份仍保留。

- ✅ 2026-09-01：V2和独立B1已接入PC独立Seq/ServiceID=0、0x0006、0x0501-0509、0x0601/0602及0x0701-0703本地业务。0x0006是通用命令执行结果，JSON使用`requestCommandId`和`status=accepted|completed|failed`，仅failed携带`errorCode`。0501走共享所有权下的非阻塞pwr_ctrl，未写入批准时序时返回`notConfigured`；0502/0505在RK安全关机确认前返回`notReady`；0506仅OFF状态写MCP4725 Fast Write，0507/0508查询最近成功配置。Apollo V2隔离网络镜像真板18/18通过；HOTPLUG另确认0701物理GPIO ODR。原App已两次备份/恢复/校验并再次心跳通过，W25未查询/写入。V2无真实TPS/PAC/AD/MCP4725且pwr_ctrl保持OFF，因此不代表真实调压、采样、Boot VOUT或RK启动通过。

- 📖 2026-08-31：用户要求IP方案及0821评审事项先保留待确认。已记录PB3/PB4/PB5在位/槽位编码区别、八槽电平表、静态/DHCP/地址范围及多B2规则、PHY侧网络切换与FX18默认路径；不改引脚或网络行为。具体待确认表在`doc/driver_tasks.md`开头；本轮按用户要求准备将V2 `main`及B1 `b1`分支发布到Gitea/GitHub，真实推送结果以提交记录为准。

- ✅ 2026-08-31：按用户要求将通用软件整合到独立B1：PC两层查表/TCP任务、RK解析与任务、UART TX修复/NORMAL RX DMA、正式监测快照、接口契约、76B版本/224B FRU v2和旧记录兼容。保留B1引脚/UID MAC/PHY/SPI4/软I2C及默认OFF，不复制测试任务/旧GPIO复位。B1扩展check（含46引脚及旧Flash模型）和UART4/USART2两配置ARM构建通过，最终暂选UART4；未烧录/联网/推送。新增C用例及B1真机验收仍待完成，详见B1 `docs/integration_20260831.md`，不能把前述旧记录中的“尚未同步B1”当作当前软件状态。

- ✅ 2026-08-31：PC帧解析也改为只读`状态 -> 字节处理函数`表：HEADER/BODY独立处理，FRAME_READY/ERROR共用拒收函数；新增状态下标/空表项防护和枚举数量编译检查。与既有`CommandID -> handler`业务表分离，TCP任务无需新增状态分支。PC离线23/23、RK模型96/96、style-test及正式/网络/Mock ARM构建通过；新增12项C状态边界检查已编译、未执行。线格式/现有Seq行为不变，未烧录/推送/同步B1。

- ✅ 2026-08-31：按RK文档审查结论修复V2畸形TLV解析：短Len、残缺头/Value、非法命令/心跳长度和TLV超量整帧拒绝；解包错误传回RK任务，避免未初始化读取和部分交付。纠正旧PC–TLV适配器、结果码“已废弃”等误导注释。Python模型96/96、PC合同19/19、style-test 19项及头编译、正式/Mock ARM构建通过；新增27项真实C隔离检查已编译、未运行，B1同步待完成。本轮不改Type/CRC/Seq/结果码、不改pending生命周期，未烧录/推送。

- ✅ 2026-08-31：V2自定义层完成一轮接口归属、中文注释和职责整理；修复UART TX锁未初始化及TXE唤醒竞态。`style-test`19项源码/模型+独立头编译通过，正式184项全量构建与6种隔离镜像通过；其余PC/RK/电源/Flash离线回归通过。本轮未烧录、未推送、未同步B1；新增8组ARM Mock用例和UART并发待V2实测，保留风险见`doc/code_quality_review.md`。

- 📖 2026-08-31：`doc/pc_tcp_protocol_spec.md`及同名Word按用户要求改为供PC工程师补充的简洁协议框架：连接方式、18字节头、B1命令编号、常用JSON和填写格式。删除草案/待定状态与MCU实现附录；实现进度仍在任务文档维护，不因框架列出命令而视为业务完成。本次仅文档修改。

**阶段：网关基线历史稳定；pwr_ctrl软件基线已建立，B1 电源硬件/采样/Boot 驱动尚未闭环。**

- 📖 2026-08-31协议确认：双方按每连接、本端发送帧独立递增Seq，不回显，接收不改变本端计数，允许同时发送和双向同号；ServiceID固定0。保留通用应答0x0006、DDR配置0x0509及电源/Boot语义；siteId/mirrorIndex含义继续保留，固件版本等后续信息追加到现有JSON，不新增查询命令。仅文档与任务更新，固件及旧网络测试尚未迁移，不得把下文历史回显记录当作当前目标合同。
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
- ✅ 2026-08-31：PC命令分发已由switch改为只读`CommandID -> handler`表，各命令使用独立处理函数并补充中文注释；PC帧状态机、ServiceID/Seq回显、每次查询读取W25和未知命令不响应的行为不变。PC离线合同19/19、RK模型65/65、正式/网络/Mock ARM构建通过；新增13项Mock运行检查仅完成编译，未烧录或联网复测。
- ✅ 2026-08-24：结果灯业务逻辑已移植B1 PG0-PG5（B1提交`f24473f`），构建通过；红绿/对象顺序和上电毛刺待真板
- ✅ 2026-08-24：B1取消温控职责，温控改为PC直连B2；B1–B2 SPI仅保活，B1基线为从机Mode 0/硬件NSS
- 📖 接手总览、暂定需求和术语：`doc/README.md`、`doc/requirements_baseline.md`、`CONTEXT.md`

**下一步（待办）**：详见 `doc/driver_tasks.md`（唯一任务来源，每任务 4 步闭环：原理图确认→编码→阿波罗V2模拟→B1/B2真板验证）
> **两阶段策略**：阶段一 = 阿波罗 V2 开发（驱动代码 + Mock + 逻辑分析仪波形验证，当前无外接模块）；阶段二 = B1/B2 真板（到货后：引脚重映射 + 实测标定 + RK/B2 MCU 联调）
1. **双协议迁移**：PC帧/cJSON、发送Seq独立递增、ServiceID=0及首批本地业务已在V2真机和B1构建闭环；下一步确认Seq首值/重连/回绕边界，建立PC业务请求到独立RK Seq的内部映射并冻结无Seq回显后的结果关联方式。MCU–RK现有TLV/pending/重传保持不变
2. LED状态语义、颜色转换、V2六路功能波形及B1 PG0-PG5软件移植已完成：B1到板后复测实际2N7002网络的上电瞬态并确认红绿/对象顺序；两颗单色状态灯语义仍待确认
3. **pwr_ctrl硬件闭环**（软件状态机与V2 GPIO波形验证已完成；待PC6/PC7顺序/延时冻结、B1真板标定和PC–MCU命令接入）— 仍阻塞电源类功能验收
4. **TPS62868/69**（A版VSET配置：VDD1/VDD2/VDDQ地址0x49/0x41/0x4F；与pwr_on联动）
5. **PAC1934T**（I2C4，驱动使用7位地址0x16）+ **AD7124**（SPI4 PE11-14，完成Mock/波形验证后待真板标定）
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
| AD7124-4 (U4028) | SPI4: PE11=NSS PE12=SCK PE13=MISO PE14=MOSI | AIN0-AIN1 ← VCC_1V8_S3_LPDDR4X / AIN2-AIN3 ← VDD2_DDR_S3 / AIN4-AIN5 ← VDDQ_DDR（导线追踪已确认, 正端各经 100R+10K+0.1uF，负端接AGND）; AIN6/7=NC | 符号1～24脚匹配TSSOP-24；FB2306=NC使REFIN1+未接VCC_2V5，固件选择内部2.5V基准 |
| PAC1934T (U204) | 原理图逻辑I2C4 (PH4=SCL/PH5=SDA，软件开漏GPIO；不是STM32 I2C4 AF引脚对) | ADDRSEL=5.23K→GND：**7位地址0x16**；总线写/读地址字节为0x2C/0x2D | SENSE1-3: VCC_1V8_S3/VDD2_DDR_S3/VDDQ_DDR 各 0.01Ω; SENSE4 未用 |
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


## 原AGENTS诊断地址记录（历史地址，不可直接复用）

- 诊断地址随链接变化，读写前必须从**实际烧录的同一份ELF**用`arm-none-eabi-nm -S -n`查询`crash_log`（16×uint32）和`tcp_diag`（8×uint32）。2026-08-31本轮正式ELF为`0x2000A57C` / `0x2000A5BC`，仅为当前构建静态结果，尚未烧录；禁止把历史地址的`-w32`命令直接套用到新镜像。`.crash_log`为NOLOAD，复位不清零。

## 任务文档中已更新的历史叙述（原文，链接按归档目录调整）

> **2026-08-31 代码规范化**：接口实例声明、RK序号共享入口和监测就绪态职责已整理，补充中文API合同；修复UART TX锁未初始化及唤醒竞态。正式全量及6种隔离镜像构建、离线检查通过；未烧录，新增Mock用例和UART并发需V2复测，尚未同步B1。审查依据与规范见[代码结构检查](../doc/code_quality_review.md)。

**B1软件整合（2026-08-31）**：PC状态表/命令表及TCP、RK/HB、monitor已进入独立B1工程；UART采用B1专用DMA/IRQ/端口绑定，保留默认OFF。W25升级76B版本/224B FRU并支持旧192B FRU/96B版本；新旧副本和有效未知格式均有保护。B1扩展静态/布局模型及UART4/USART2两配置ARM构建通过，未真机运行；9.5/9.6/9.7/9.9业务合同和前序V2 C用例仍未完成。详情见B1工程`docs/integration_20260831.md`。

**协议确认（2026-08-31，文档已更新、代码未迁移）**：每条连接中双方分别维护发送Seq，每发一帧本端加1；接收不改变本端计数、不回显请求Seq，两个方向可同时发送且可出现相同数值。ServiceID保留2字节、固定0、不参与分发。框架新增0x0006通用应答、0x0509 LPDDR类型/三轨调压配置入口；Boot设置仅供下次启动、查询返回配置值。下述8月27日及查表回归记录均描述旧回显实现，不代表新合同已经验收。siteId/mirrorIndex含义继续保留；固件版本等信息后续追加到现有信息JSON，不必新增命令。

**当前状态（2026-09-01）**：PC–MCU使用固定18字节大端头部+UTF-8 JSON，MCU–RK继续使用0x5A/0xA5 TLV。两套源码物理隔离，PC状态机/JSON命令表/TCP socket三层分离；ServiceID固定0，MCU使用连接内独立发送Seq。0x0006、0x0503/0504、0x0509、0x0601/0602及0x0701/0702/0703已接入V2/B1；Apollo V2网络业务12/12及六路GPIO ODR实测通过。本地测试仪信息仍从W25 FRU读取，本轮真机脚本特意不查询以避免写入。PC业务到RK Seq的内部映射尚未接入，Length上限仍为4096字节。

**查表分发维护方式（2026-08-31）**：在`pc_dispatch.c`添加命令处理函数，并在`s_command_table`登记唯一CommandID；不需要修改TCP任务或字节流状态机。入口统一解析/释放cJSON，handler只借用请求对象；未命中时仍校验JSON并返回`NO_RESPONSE`，不自动ACK或转发RK。命令表中明确拒收0x0001/0x0102反向响应。离线合同检查路由及重复命令号，`driver_mock_test.c`新增13项检查覆盖非法包体、方向错误、START_TEST、未知命令、旧响应长度清零和空参数；本轮只编译这些C运行用例，不计作已在MCU执行通过。

**帧状态查表维护方式（2026-08-31）**：`pc_state_machine.c`的`s_state_handlers`以状态枚举为下标；HEADER负责第4/6/8字节字段校验和第18字节转态，BODY只收正文，两个终态共用拒收函数。新状态在`PC_SM_STATE_COUNT`之前声明并登记处理函数；不得把CommandID、JSON或socket逻辑搬进状态函数。PC离线增加空正文边界、4KiB满帧、早拒绝时点和四态路由检查，合计23/23；隔离Mock新增12项真实C检查，已编译未运行。状态查表不代表任务9.6的Seq新合同已经实现，也不代表PC-RK业务已接通。
