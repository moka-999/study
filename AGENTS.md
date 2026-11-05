# B1 project rules

FUNC0103、PARA0108、SLT010A均必须携带TimingParameters对象；默认时序传{}，缺失或null回invalidArgument。16个子字段可部分提供字符串：CASReadLatency/CASWriteLatency/Freq/tREFI/tWR/tRTP/tRCD/tRP/tRASmin/tRASmax/tWTR/tRFCmin/tFAW/tCCD/tRRD/BL。Freq为int64，其余为u32十进制整数；PC判定取值及适用性，MCU只检查格式/位宽并原值编码。空串/省略不下发普通时序，Freq缺省回复-1；不继承上一请求。Freq变化走既有冷启动并在0x35查询中回复，普通时序在SPL就绪后无应答逐项发送，再下发算法；发送完成不代表生效。见[接入记录](../test/doc/rk_timing_frequency_20261105.md)。旧请求补字段后必须重算Length。

## RK协议替换约束（2026-09-23）

- 用户指定RK工程师的`D:/pro/tlv_proto`为基线；与V2共享`frame/state_machine/rk_link/rk_task`，保留B1的UART4/USART2、DMA、OFF门禁和任务存活监测，不引入V2板级GPIO。
- Type/Length/Seq大端、Value整数和CRC小端；HCRC覆盖前3字节，FCRC覆盖Seq＋TLV。普通心跳Length=0、零TLV、16字节；接收兼容0x0000空Value。非空Length为4..1024，1..3非法。
- RK运行期上行填最近收到的MCU Seq；MCU只按本次命令至最新成功入UART队列Seq过滤，允许滞后/重复，完整结果还须匹配单在途算法。旧结果被重标当前窗口Seq且同算法时无法区分。启动频率0x35独立回复5001+801C/int64；上电过渡仅开放此查询，普通时序在SPL就绪后发送5000+701B/u32，不等设置ACK，见`../test/doc/rk_timing_frequency_20261105.md`。
- 进度严格为5001算法＋3016进度，0..100仅参考。最终为单个3/5/7 TLV包：5001/1017/9002各一次，0～2组相邻9001+3001明细。零地址表示该颗PASS，非零才计入失败数组；die仅0/1且唯一（含PASS明细），1017须与是否存在非零失败地址一致。整包、Seq和算法通过才一次交付；旧四详情/独立FAIL不再汇总，错误包保持busy，无重发。见`../test/doc/rk_protocol_review_20261105.md`。
- `rkRequestQueue/rkResponseQueue`各8槽，只允许单个在途测试；无500ms自动重传。最终结果超时后暂停后续发送，直到匹配迟到结果或B1断电；队列满不丢完成结果。
- 1..20为参考文档定义的无参数功能测试，17=MATS；PARA的21..36按idd_0等名称接入编号下发，进入/退出及有效测量窗口待接。0103/0108允许操作者已确认RK测试程序就绪、MCU电源状态为ON的外部启动路径：power/bootMode均null、reboot=false。完整启动另接正式电源编排，缺配置/RK确认仍拒绝；不移植V2模拟旁路。外部路径见`docs/real_test_sync_20260929.md`，完整编排见`../test/doc/power_sequence_20261012.md`。
- MCU→RK心跳周期为1000ms（1秒），由同一RK任务推进；B1非ON时不发送，发送失败或错过周期不补发。当前RK收帧后空闲回零TLV心跳、在途回进度，最终结果可主动发送；MCU不再对RK应答回心跳。PC心跳仍为请求/应答，不受此周期控制。
- PC 0105顶层siteId标识B1，measurementData是共享统计；testResults固定两项mirrorIndex=0/1，非零失败明细按die匹配，零地址及未列者PASS。testStatus为0=FAIL/1=PASS；PASS地址null，FAIL保留16位非零十六进制地址，真实errorType为空。physicalAddr保留空串，B1始终真实、禁止MATS假FAIL。
- RK字节间超时与V2统一为100ms；仅丢弃超时半帧，不改变算法等待或Seq窗口。B1构建拒绝ENABLE_V2_MATS_FAIL_TEST、电源模拟及旧网关联调开关。
- 通用同步及离线复跑入口见`../test/doc/v2_b1_sync_20261010.md`；板型差异不靠整目录覆盖消除。
- B1结果灯不作RK通信指示，PC温控边界不变。代码/离线/构建与B1实板验收分开记录，见`docs/rk_protocol_migration_20260923.md`。
- 整轮先0106 `{}`；轮内FUNC用0103/0105，PARA用0108/0109，SLT用010A/010B，最后0107提交errorchip数组。镜像0/1/2分别映射LPDDR0/LPDDR1/eMCP，其他编号整包拒绝；070x已删除，合法旧请求回notSupported。0108沿用FUNC开始包字段、类别为parametric，0109只允许MCU方向。0107不取消RK或提前释放占用，只阻止尚未入队的FUNC/PARA及废弃未交付结果。SLT只匹配slt/memtest86，index=null，默认空时序按参数准备电源/Boot；非负Freq等待本轮查询回复，非空普通时序等SPL普通心跳后发送，不检查EDK；原连接010B确认实际结束，断线后不自动认领旧SLT。0106按B2已知镜像模式显示常温/高温闪，未知灭；轮内局部模式更新显示，0107后仅缓存，不覆盖最终PASS/FAIL。

## 20260920-2134原理图适配边界

- PAC1934三路R7268/R7269/R7270均为20mΩ，生产采样必须显式使用板级20000µΩ，不能采用通用驱动的10mΩ兼容值。0602复用现有采样结果，不在JSON层再次乘除系数。
- TPS料号、地址不变；U4025为56.2kΩ，U4026为86.6kΩ。steps采用原生步进：VDD1每档10mV、VDD2/VDDQ每档5mV，相对标称值且各256档。TPS输出选装和实物阻值仍需验收，寄存器写成功不证明DDR实际已由TPS供电。
- 用户接受共同4V上电后能及时完成调压的开发预设，暂不新增RK初始化前等待机制；B1已按要求装载可调轨间初值，仍须实板验收，不跳过初始化后的RK-ready，保持默认OFF及业务配置门禁。
- 0103完整启动已接成功配置比较、真实训练上电/Boot驱动与冷重启。初版按用户确认以本轮CRC有效普通心跳表示RK初始化完成，直接下电不等RK关机应答；PC确认DDR安全范围，MCU保留格式/TPS范围/原生步进校验，不夹紧或取整。进度/结果/文字/旧心跳不能放行；启动参数不是硬件验收，RK须保证发心跳时可执行算法。见`../test/doc/b1_power_business_20261013.md`。
- Boot实际写失败后缓存失效，0507返回notConfigured；参数/忙/状态拒绝不改旧缓存。查询仍不是DAC模拟电压回读。H13首次上电/掉电输入容限待硬件/RK确认，不自动写EEPROM或改成全时0V。
- 适配及复跑记录见`docs/schematic_adaptation_20260921.md`；离线验证不能替代实板验收。

## 精简Boot＋单App下载（2026-10-28）

- Boot=0x08000000/128KiB，App=0x08020000/1920KiB；合并ELF/BIN一次SWD下载，无OTA、双槽、回滚、30秒确认。0901～0903仍保留编号并回notSupported。
- Ctrl+Shift+B或`tools/dev.ps1 flash -ConfirmIdle YES`自动编译、内部Flash备份、下载、回读和复位；`build`只编译。产物`build/standalone/Debug/b1.elf|bin`包含Boot＋App；不需要同级Bootloader。B1禁止V2模拟/假FAIL。
- SWD Normal/SWrst/950kHz适配四线J5：1=GND、2=SWCLK、3=SWDIO、4=3V3_MCU参考，无NRST，板卡自行供电。下载先停止RK/温控并断开PC。Boot及App均保留RAM初始化前关PC6/PC7/PG11；不修改Option Bytes，不全擦。
- W25装配按目标板分别确认：当前验板目标未焊MCU W25Q256，`Core/Inc/board_storage_config.h`默认`BOARD_W25_PRESENT=0`，Boot和App跳过器件，0102使用宏定义信息。已装器件板须改1并重编、下载完整Boot＋App，恢复Boot检查/准备FRU、App只读；启用器件时初始化或FRU失败会停在Boot，不进入App。具体镜像及验收状态见同级test的任务清单，不能按IP推断是否装W25。
- 已装W25时，Boot初始化/迁移FRU并回读，App只读；有效记录不重复擦写，未知有效格式不覆盖。固件版本、旧MCU包/启动日志及RK两块8MiB预留区不访问。无OTA安装器，不把外部固件烧入内部Flash。
- 各版本的构建、安装与实板验收分别记录在`../test/doc/driver_tasks.md`；操作和限制见`../test/doc/direct_flash.md`及`../test/doc/minimal_boot_20261028.md`，已安装不代表全部硬件验收通过。

## 板级约束

- Target: STM32H743IIT6, LQFP176, 25 MHz HSE, 400 MHz core.
- 精简Boot从`0x08000000`启动，单App位于`0x08020000`、容量1920KiB；VTOR使用g_pfnVectors。
- PHY is YT8512H; reset is PG12 `ETH_nRST`.
- The current BRD power topology was accepted on 2026-09-01: PC6 is the master
  switch for VCC4V0_SYS and all three TPS/DDR rails, PC7 controls 3.3V, and the
  PG11-to-TPS links R4073/R4081/R4086 remain NC while TPS EN is pulled up to 4V.
  Firmware drives PG11 as a defensive optional group gate: low during reset,
  OFF and faults, and high only after both system-rail enables have been issued.
  This has no electrical effect while the links are NC and must never replace
  PC6 as the reliable shutdown path. TPS voltage adjustment remains available
  by I2C after PC6 has powered the converters.
- 已确认耦合复位：每次MCU复位（NRST、看门狗、软件或调试器）都须关闭RK/DDR。启动汇编Reset_Handler在RAM初始化前拉低PG11、PC6、PC7，App保持OFF，PC随后重新执行正常上电流程；不能删除或延后到main才关电。
- 看门狗由`Core/Inc/board_watchdog_config.h`的`BOARD_IWDG_ENABLE`集中控制：当前验板默认0，不启动IWDG、不创建喂狗任务；改1恢复四任务监督。宏不改变Option Bytes，不能停止旧固件已开启的IWDG；首次换入仍需处理下载时的调试冻结。V2配置不随本次B1调整。
- SPI2为B1从机，PB12～15使用硬件NSS低有效、Mode0、8位MSB；B2共享主机总线及8个独立CS，初始1MHz。0004统一状态交换包含可选testerId/镜像显示模式，周期约1秒、3秒离线诊断；不控制或转发加热。SPI4仍是AD7124 Mode3主机。
- `App/tasks/b2/`独立任务及业务队列，`App/protocol/b2/`独立解析/FSM；不得混入RK任务、队列或Seq。仅共用`App/protocol/common/crc.[ch]`纯算法。TCP上下文应用IP/整轮灯态，lwIP线程真正设置地址；SPI DMA1 Stream3/4、`.b2_dma`位于AXI SRAM，不使用UART的Stream0～2。
- B1 has no temperature-control role. PC communicates directly with the B2 MCU;
  the B2 firmware owns heating and all other backplane-level control.
- PH4/PH5 are the schematic's logical I2C4 bus and must remain open-drain GPIOs
  for software I2C; do not configure the invalid `GPIO_AF4_I2C4` mapping.
- UART4固定连接RK UART1_M0运行协议；USART2双向连接RK UART2_M0_DEBUG；USART1在MCU模式接收本地命令，RK模式转发输入，行首log mcu加换行截获返回MCU。UART4协议不参与调试转发。
- USART1在MCU模式接受`start rk`/`end rk`加换行：前者以LPDDR4X标称1800/1100/600mV、fspiM1走现有冷启动，已ON也先正常下电；后者正常下电。请求由唯一TCP业务上下文推进，无需PC连接；测试/电源在途返回busy，不建立SLT占用、不等RK心跳，power ON不证明RK启动成功。RK模式须先log mcu；详细回复与离线验证见`docs/uart_log_console_20261010.md`。
- `b1.ioc` is the CubeMX hardware baseline. PA13/PA14 are explicitly reserved for
  SWD and PH4/PH5 start high/released; preserve both settings. After regeneration,
  保留Boot首128KiB、App起点`0x08020000`/容量`1920K`，VTOR使用`g_pfnVectors`。
- Portable V2 modules may be integrated after their build/offline checks, as
  requested on 2026-08-31. Integration is not hardware acceptance: retain every
  unrun C test, network/UART check and B1 real-board calibration item explicitly.
- AD7124-4仅绑定B1 SPI4，Mode3、约390.625kHz；ON后才初始化，实板ID为0x07。
  三路保持AIN0/1、2/3、4/5，负端为板上AGND，不改为内部AVSS。
  增益1、双端无缓冲，内部校零后保留出厂GAIN；接地负端UV策略见排查记录。
  当前按用户确认采用AVDD基准，关闭内部REF_EN；板级开关及实测3333mV集中在
  `Core/Inc/board_adc_config.h`，同步/增量采样共用换算值。AVDD不是稳定基准承诺，
  更换板卡或供电条件需复测，精度待与万用表验收。内部2.5V方案保留但须先补齐
  REFOUT到AVSS的0.1µF去耦；不直接补焊FB2306，避免并联内外基准。
- The V2-accepted MCP4725 driver is bound to B1 hardware I2C2 at address 0x60.
  It uses Fast Write only and must not write EEPROM. Do not set a Boot voltage
  automatically before B1 output-voltage and RK boot-mode validation.
- TPS62868/PAC1934/DDR通用驱动已迁入；VDD1/VDD2/VDDQ分别使用I2C1、I2C2和PH4/PH5软件I2C。
  `pwr_ctrl_init(&b1_power_default_timing)`拉低PC6/PC7/PG11并保持OFF，不在初始化时访问TPS。
  联调初值集中在`Core/Inc/board_power_config.h`：PC7先开→10ms→PC6开→5ms后写TPS；
  正常下电先关PC6，10ms后关PC7；故障/启动取消立即全关。初值不是批准时序，
  实际C离线验证及首板反灌/训练窗口验收见`../test/doc/b1_power_defaults_20261013.md`。
  独立有符号原生档位：VDD1为10mV，VDD2/VDDQ为5mV，覆盖TPS全部寄存器码。
  极端值并非DDR安全电压，禁止自动遍历；内部原生档位不是PC线上字段，PC统一使用字符串mV。
- Training `power` means the voltages used before and during DDR initialization,
  not a post-training offset. `monitor_request_training_power_on()` copies an
  explicit three-rail target under the existing mutex and accepts it only in OFF;
  `ddr_power_init()` writes that target directly, stops at the first I2C error,
  and updates its voltage cache only after all writes succeed. Runtime bias does
  not alter the training target; selecting only a memory type restores nominal
  training voltages. The target cache is not a successful-training record.
  Development assumes the RK806 automatic startup window is sufficient, as
  accepted on 2026-09-14; this is not measured timing approval. Keep default OFF,
  preserve the B1-specific PC6/PC7 sequence and optional PG11, and do not copy V2
  waveform delays. 新PC字段及完整编排已接入；板级时序与业务等待已有联调初值，
  main绑定本轮普通心跳就绪，安全训练范围由PC确认，不把离线通过当成成功训练。
- W25Q256 now uses the 76-byte firmware payload and 224-byte FRU v2. The MCU-side
  device in the 2026-08-27 BRD is U2 on 3V3_MCU/QSPI-BK1; U4007 is RK-side FSPI
  and must not be used as MCU Flash evidence. Keep read
  compatibility for 96-byte firmware and 160/192-byte FRU records; preserve old
  identity/network fields and refuse to overwrite valid unsupported formats.
  已装W25时只有Boot初始化/迁移FRU；App加载只读，不同步固件版本，不在查询时写入。
  未焊首板通过BOARD_W25_PRESENT=0显式跳过；补焊后恢复Boot写、App读。
- FreeRTOS 10.6.2 and LwIP 2.2.1 have been migrated for the final gateway using
  B1's existing YT8512 driver, UID-derived MAC and PG12 PHY reset. Formal TCP,
  RK (including periodic heartbeat) and power-gated monitor tasks are integrated; never copy V2 board glue.
- The PC state-handler table and command-handler table are attached to the TCP
  server, using cJSON v1.7.19. The wire header is 18-byte big-endian
  plus a UTF-8 JSON object; the u32 Length field has a confirmed 4096-byte MCU
  PDU limit. cJSON uses pvPortMalloc/vPortFree and nesting depth 16. PC and MCU
  now use independent per-connection u32 send counters with ServiceID fixed zero:
  first Seq is 1, successful sends advance it, reconnect resets it to 1, and
  UINT32_MAX wraps to 0. Request/RK Seq never enters an MCU send header.
- 测试仪查询0101/0102及空对象心跳已接入。0103/0108必须携带算法大类/名称/index、
  power、reboot及bootMode。外部就绪模式严格使用null/false/null；通过RK命令表
  按类别查找FUNC 1..20或PARA 21..36，不猜未知名称。正式TCP领取完整3/5/7 TLV结果并发送0105或0109，包含testTimeMs、
  首错地址及实际采样的18项均值/峰值/最小值；无有效样本报null，不造假零值。
  真正投递算法前重开统计，不计冷启动等待；结果领取后冻结，0601只读查询不因算法在途返回busy。
  断线不撤销算法，新连接不领取旧0105/0109。电流快速独立采集，AD保留约240ms三轨高精度扫描；非JEDEC同步IDD窗口。101A动作已另备内部IDD会话：实际发完进入帧后默认等5ms、逐点保存5s电流、冻结后发退出；正式0108未切换，判限/JSON及UART完成/监测事件绑定待接，见同级test/doc/idd_session_prepare_20261105.md。7018寄存器地址u32LE仍仅编解码准备。
  统计内部保留µA，上报mA允许小数；缓存不重复计数，见同级test/doc/sampling_schedule_20261026.md。
- PC heartbeat is frozen as client request 0x0002 with `{}` and server response
  0x0001 with `{}` using the MCU connection-local send Seq.
- The local business now includes 0x0501/0x0502 power requests, 0x0503/0x0504
  state, 0x0505 reset gate, 0x0506/0x0507/0x0508 Boot configuration/query,
  0x0509 memory-type/voltage and 0x0601/0x0602 cached monitoring.
  结果灯已迁至0106/0107，070x命令入口已删除。0x0509使用
  固定四字段`memoryType+vdd1+vdd2+vddq`：三路电压是绝对mV十进制字符串，
  必须满足标称值加原生步进乘整数及TPS范围，不自动取整；旧steps字段不再接受。
  0103.power复用同一解析器。2026-11-04起B1的0509在OFF接受可编码目标，
  仅保存RAM训练目标，下一次0501上电写TPS；MCU复位后恢复标称。
  ON只允许当前类型实时调压，不覆盖训练目标，仍须PC确认RK完成DDR初始化。
  内部使用10/5/5mV有符号档位；0501在ON时不重启，目标缓存不证明物理启动时序。
  采样、Boot I2C、调压和pwr_ctrl推进共用监测层事务锁。0501/0502/0505及完整0103
  已由pc_power_service分阶段推进，原任务不变；main同时装载轨间和业务初值。
  0502直接关电；0505/完整0103上电后等待本轮有效普通心跳，超时报告timeout，
  不自动关电或重试。RK心跳发送时点、电气时序及直接掉电存储风险待验收。B1阶段日志走log_console，
  不往RK协议口打印，也不增加PC字段。
- 0x0006 is the generic command execution result, not an error-only response.
  It uses decimal `requestCommandId` and `status=accepted|completed|failed`;
  `errorCode` exists only for failed results and may be
  `invalidArgument|invalidDirection|notSupported|invalidState|busy|notReady|notConfigured|executionFailed`.
  非法头前缀逐字节丢弃，累计超过4096字节垃圾或半帧空闲500ms才断连；
  完整坏UTF-8/JSON回0006 invalidArgument后继续，不在正文内搜索控制帧。
  JSON词法检查拒绝原始NUL、未转义控制字符及`\u0000`，防止C字符串截断。
- B1 and V2 both keep D-Cache disabled. ETH DMA memory belongs in D2 SRAM and
  the FreeRTOS heap belongs in AXI SRAM; never add SCB cache maintenance calls.
- PG0-PG5仅用于三颗双色结果灯：奇数LED1/3/5为FAIL通道，偶数LED2/4/6为PASS通道；常温测试双通道闪烁，高温测试仅FAIL通道闪烁，亮灭各500ms。图纸料号L-C295JFLBCT的手册为琥珀/蓝，实际灯色及LPDDR0/LPDDR1/eMCP物理顺序等真板确认，不按图面红绿旁注改业务。
  PC不再单独设置灯态；0107按失败芯片所属mirrorIndex汇总，0/1/2对应两组LPDDR/eMCP。
- PG6/LED7已确认为MCU正常上电指示：GPIO初始化先低，默认任务完成串口及LwIP初始化后置高常亮，不跟随结果灯、网络连接或RK/DDR电源状态闪烁；不代表整板电压或RK就绪。详见`docs/board_status_slot_20261010.md`。
- Do not copy V2 pin mappings, PCF8574 PHY reset, fixed MAC or test scripts.
- `B1_RK_UART=4`已固定，CMake拒绝2；逻辑slot0=UART4协议、slot1=USART2日志、slot2=USART1控制台，后两路绝不进入RK解析器。USART2独立TX发送环、0等待取锁；调试输入拥塞暂存一块，RX丢失报告并丢弃至换行。
- UART4 PA11 RX/PA12 TX经U4008到RK UART1_M0（GPIO2_B7 TX/GPIO2_B6 RX），USART2 PD5/PD6经U4013到RK UART2_M0_DEBUG，USART1 PA9/PA10为本地日志出口。映射按20260920-2134原图第6/14页核对，纠正旧UART0记载；三路当前115200/8N1，协议线路仍待实板验收。
- RX为NORMAL DMA1 Stream0/UART4、Stream1/USART2、Stream2/USART1，IRQ优先级5；HT忽略，IDLE/TC交给各自唯一消费者。队列在调度前注册，uart_start_all()在调度后启动。
- 日志复用默认任务，不新增任务；编译`B1_LOG_DEFAULT_SOURCE=MCU|RK`指定上电默认，运行时USART1文本`log mcu`/`log rk`加换行选择，重启恢复默认且不写Flash。仅一来源输出，运行期MCU printf经log_console统一过滤，禁止直接写huart1。唯一调度前例外为board_diagnostics：USART1初始化后、内核对象创建前按MCU默认来源输出有界启动阶段；进入内核前永久关闭此出口。USART1新增status只读查询，由默认任务读取缓存并限速输出，不触发采样、上电或修改协议。USART1取TX锁为0等待，拥塞允许丢日志，不能阻塞协议或业务；说明见`docs/uart_log_console_20261010.md`。
- Keep `.uart_dma` in AXI SRAM after the RTOS heap. Reserve the fixed 30KiB LwIP
  allocator plus metadata in `.lwip_heap` at 0x30005000 (32KiB reserved), so ETH
  pools cannot silently overlap it. Never put UART DMA in DTCM.
- 保持默认OFF门禁：pwr_ctrl到ON后才允许RK命令/心跳和采样。初版直接关电，
  不再把安全关机握手列为执行前提；已入UART队列的字节撤销仍未实现。
- B1 result LEDs are not TCP/RK debug indicators; never borrow PG0-PG6 for V2
  connection/heartbeat toggles. Exclude the old TLV GPIO reboot dispatcher.
- See docs/integration_20260831.md for migrated scope and remaining acceptance.
- PB3/PB4/PB5为GPIO输入、GPIO_NOPULL；B1各有4.7kΩ上拉，B2贴0Ω接地为0、不贴为1，不允许MCU输出高。`board_slot_code()`返回PB5:PB4:PB3的0..7码，加1为siteId；111有效，独立的SLOT_ONLINE#才是B2在位检测。每次0004重新读取槽位，实际八槽贴装/片选仍待验收。
- B2通过0004通知testerId=1～21，成功配置后IP为`192.168.210.(10*testerId+siteId)`，PC结果siteId使用同一槽位字符串。不同编号/槽位动态切换，但已有PC连接或测试/电源在途则整包busy；同IP不重配。掩码255.255.255.0、网关0.0.0.0，RAM保存、不写FRU；首次配置前保留本地192.168.1.201，正式多板上线仍须处理默认IP冲突隔离。见`../test/doc/b2_spi_integration_20261020.md`；本轮仅离线/构建通过，未实板验收。
- Build with `.\tools\dev.ps1 build`; run `.\tools\dev.ps1 check` before commit.
