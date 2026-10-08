# B1 project rules

## RK协议替换约束（2026-09-23）

- 用户指定RK工程师的`D:/pro/tlv_proto`为基线；与V2共享`frame/state_machine/rk_link/rk_task`，保留B1的UART4/USART2、DMA、OFF门禁和OTA健康上报，不引入V2板级GPIO。
- Type/Length/Seq大端、Value整数和CRC小端；HCRC覆盖前3字节，FCRC覆盖Seq＋TLV。普通心跳Length=0、零TLV、16字节；接收兼容0x0000空Value。非空Length为4..1024，1..3非法。
- RK所有上行均填写RK最近收到的MCU Seq，不回显原测试Seq，不使用RK自身心跳计数；两包Seq可不同。MCU只按本次命令至最新成功入UART队列Seq过滤，允许传输滞后，不要求精确等于最新值。单在途匹配详情算法后汇总；旧结果若被RK重新标上当前窗口Seq，同算法时无法区分。
- 0x3016为1字节进度0..100，仅缓存参考、不判断完成/ready。四详情0x5001/0x9001/0x3001/0x9002同帧且各一次；另收0x1017的1字节bool（0=PASS、1=FAIL），收齐才上报一次0105；地址不判PASS，缺包保持busy，不自动重发。相同重复幂等，重复内容矛盾保持等待；见`docs/rk_protocol_update_20260930.md`。
- `rkRequestQueue/rkResponseQueue`各8槽，只允许单个在途测试；无500ms自动重传。最终结果超时后暂停后续发送，直到匹配迟到结果或B1断电；队列满不丢完成结果。
- 1..20为参考文档定义的无参数功能测试，17=MATS；IDD21..36不支持。0103允许操作者已确认RK测试程序就绪、MCU电源状态为ON的外部启动路径：power/bootMode均null、reboot=false。完整上电/训练/重启请求仍拒绝；不移植V2模拟旁路。详见`docs/real_test_sync_20260929.md`。
- MCU→RK心跳周期为1000ms（1秒），由同一RK任务推进；B1非ON时不发送，发送失败或错过周期不补发。PC心跳仍为请求/应答，不受此周期控制。
- B1结果灯不作RK通信指示，PC温控边界不变。代码/离线/构建与B1实板验收分开记录，见`docs/rk_protocol_migration_20260923.md`。

## 20260920-2134原理图适配边界

- PAC1934三路R7268/R7269/R7270均为20mΩ，生产采样必须显式使用板级20000µΩ，不能采用通用驱动的10mΩ兼容值。0602复用现有采样结果，不在JSON层再次乘除系数。
- TPS料号、地址、20mV步进不变；U4025为56.2kΩ，U4026为86.6kΩ。TPS输出选装和实物阻值仍需验收，寄存器写成功不证明DDR实际已由TPS供电。
- 用户接受共同4V上电后能及时完成调压的开发预设，暂不新增RK初始化前等待机制；这不批准具体毫秒延时或跳过初始化后的RK-ready。保留默认OFF和未配置时序门禁。
- 0103保留完整参数校验及带合法index的失败应答；仅外部就绪且ON路径可交给真实RK。安全训练范围、成功配置比较、自动冷重启及自动RK-ready仍未实现，不得把本路径宣称为完整上电测试闭环。
- Boot实际写失败后缓存失效，0507返回notConfigured；参数/忙/状态拒绝不改旧缓存。查询仍不是DAC模拟电压回读。H13首次上电/掉电输入容限待硬件/RK确认，不自动写EEPROM或改成全时0V。
- 适配及复跑记录见`docs/schematic_adaptation_20260921.md`；离线验证不能替代实板验收。

## OTA增量约束

- 空白板首部署用`tools/first_flash.ps1 -Action build -Version <版本> -RkUart 4`（或2），输出固定B1包；共用独立Boot首烧工具，先备份/全空检查，B+A1与恢复包回读通过后才提交日志，无force、不覆盖FRU。B1禁止使用V2包/模拟配置，新首烧编排仍待实板验收；见`../bootloader/README.md`。

- 普通构建保持OTA关闭；`ENABLE_OTA_BOOT=ON`必须同时启用槽限/修正表构建，A1/A2共同上限896KiB，向量使用`g_pfnVectors`符号随镜像修正。
- 完整B在独立bootloader分支用`BOOT_BOARD=b1`生成，板型固定b1-h743；复位在RAM初始化前关PC6/PC7/PG11。禁止使用V2的B/包或普通最小跳转B覆盖已部署OTA的板卡。
- 下载只在已健康且OFF时进行；默认、TCP、监测、RK四个必要任务（包括OFF的RK）上报进展，30秒健康才确认。心跳随RK任务推进，不再创建独立HB任务或要求HB健康位。Flash查询、下载及启动日志共用事务锁，保留B1的FRU迁移/UID MAC规则。
- PC固件升级0901–0903使用通用枚举名；MCU仅接受`mcu_begin/mcu_write/mcu_finish/mcu_query/mcu_cancel`，对应五个`rk_*`动作仅返回`notSupported`且不得进入MCU下载或查询。W25Q256为RK预留两个独立8MiB空间，仅固定地址/容量，不定义RK包体、SPL/EDK分配或回滚语义；见[PC固件升级协议](docs/protocol/pc/pc_ota_protocol.md)。
- 同步及复跑见`docs/ota_sync_20260916.md`；B1目前仅构建/离线验证，不能引用V2实测冒充B1实测。

- Target: STM32H743IIT6, LQFP176, 25 MHz HSE, 400 MHz core.
- App starts at `0x08020000`; `boot.elf` starts at `0x08000000`.
- PHY is YT8512H; reset is PG12 `ETH_nRST`.
- The current BRD power topology was accepted on 2026-09-01: PC6 is the master
  switch for VCC4V0_SYS and all three TPS/DDR rails, PC7 controls 3.3V, and the
  PG11-to-TPS links R4073/R4081/R4086 remain NC while TPS EN is pulled up to 4V.
  Firmware drives PG11 as a defensive optional group gate: low during reset,
  OFF and faults, and high only after both system-rail enables have been issued.
  This has no electrical effect while the links are NC and must never replace
  PC6 as the reliable shutdown path. TPS voltage adjustment remains available
  by I2C after PC6 has powered the converters.
- Coupled reset was explicitly confirmed on 2026-09-01: every MCU reset (NRST,
  watchdog, software or debugger) must shut down RK/DDR. `boot/boot.s` therefore
  drives PG11, PC6 and PC7 push-pull low before App validation; an invalid App
  remains powered off, and PC must explicitly run the normal power-on sequence.
- SPI2 is the B1 slave on B2's shared master bus: mode 0, 8-bit MSB and hardware
  NSS low. B2 selects eight B1 slots with independent CS GPIOs and initially
  clocks at 1 MHz. This link is keepalive-only: never add temperature-control
  forwarding or other business commands. Keepalive payload, cadence and timeout
  remain deferred to the B2 joint review. SPI4 is the AD7124 mode-3 master.
- B1 has no temperature-control role. PC communicates directly with the B2 MCU;
  the B2 firmware owns heating and all other backplane-level control.
- PH4/PH5 are the schematic's logical I2C4 bus and must remain open-drain GPIOs
  for software I2C; do not configure the invalid `GPIO_AF4_I2C4` mapping.
- Final RK links are UART4 and USART2. USART1 is local console only.
- `b1.ioc` is the CubeMX hardware baseline. PA13/PA14 are explicitly reserved for
  SWD and PH4/PH5 start high/released; preserve both settings. After regeneration,
  restore the app linker origin `0x08020000`/length `1920K` and vector offset
  `0x020000`.
- Portable V2 modules may be integrated after their build/offline checks, as
  requested on 2026-08-31. Integration is not hardware acceptance: retain every
  unrun C test, network/UART check and B1 real-board calibration item explicitly.
- The V2-accepted AD7124-4 driver is ported and bound only to B1 SPI4. SPI4
  starts in mode 3 at about 390.625kHz; do not auto-run `ad7124_init()` before
  the first B1 board can provide a real ID and voltage-reference check. The
  20260831 schematic leaves FB2306 NC, so B1 initialization selects the internal
  2.5V reference instead of the disconnected REFIN1 input. Its three voltage
  channels follow the schematic pairs AIN0-AIN1, AIN2-AIN3 and AIN4-AIN5;
  AIN1/3/5 are the board AGND returns and must not be replaced by internal AVSS.
  At gain 1 only the positive input buffer is enabled: the grounded negative
  pins violate the buffered AVSS+100mV headroom, and the internal reference
  does not need the external-reference buffers.
- The V2-accepted MCP4725 driver is bound to B1 hardware I2C2 at address 0x60.
  It uses Fast Write only and must not write EEPROM. Do not set a Boot voltage
  automatically before B1 output-voltage and RK boot-mode validation.
- The V2-accepted TPS62868/PAC1934/DDR power baseline is ported without startup
  device access. VDD1/VDD2/VDDQ use I2C1/I2C2/PH4-PH5 software I2C respectively;
  `pwr_ctrl_init(NULL)` reinforces PC6/PC7/PG11 low and leaves
  power-on prohibited until real-board timing is frozen. Runtime voltage offsets are
  independent signed 20mV steps across the TPS-register writable range; these
  extreme values are not DDR-safe and must never be swept automatically.
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
  waveform delays. 新PC字段已完成严格解析；安全训练限值、配置变化后的冷重启
  编排及RK就绪确认仍待接入，不能把字段解析当成成功训练。
- W25Q256 now uses the 76-byte firmware payload and 224-byte FRU v2. The MCU-side
  device in the 2026-08-27 BRD is U2 on 3V3_MCU/QSPI-BK1; U4007 is RK-side FSPI
  and must not be used as MCU Flash evidence. Keep read
  compatibility for 96-byte firmware and 160/192-byte FRU records; preserve old
  identity/network fields and refuse to overwrite valid unsupported formats.
  Startup initializes QSPI but never auto-writes production metadata sectors.
  Runtime PC information comes from W25 loads, not firmware_version_current().
  Until Boot/factory writing exists, only a query may initialize blank/corrupt
  FRU or migrate a supported old record, followed by a verified readback.
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
- 测试仪查询0101/0102及空对象心跳已接入。0103必须携带算法大类/名称/index、
  power、reboot及bootMode。外部就绪模式严格使用null/false/null；通过RK命令表
  查找1..20，不猜未知名称。正式TCP领取详情与FAIL已收齐的结果并发送0105，包含testTimeMs、
  首错地址及实际采样的18项均值/峰值/最小值；无有效样本报null，不造假零值。
  每次受理重开统计，结果领取后冻结；0601只读查询不因算法在途返回busy。
  断线不撤销算法，新连接不领取旧0105。采样仍是后台约2秒加IO间隔，非JEDEC同步IDD窗口。
- PC heartbeat is frozen as client request 0x0002 with `{}` and server response
  0x0001 with `{}` using the MCU connection-local send Seq.
- The local business now includes 0x0501/0x0502 power requests, 0x0503/0x0504
  state, 0x0505 reset gate, 0x0506/0x0507/0x0508 Boot configuration/query,
  0x0509 memory-type/20mV steps, 0x0601/0x0602 cached monitoring, and
  0x0701/0x0702/0x0703 indicators. 0x0509 is
  exactly `memoryType+vdd1Steps+vdd2Steps+vddqSteps`: OFF accepts only all-zero
  steps to select type. ON applies validated steps only to the selected type;
  PC must issue nonzero adjustment after RK DDR initialization.
  Sampling, Boot I2C, voltage writes and pwr_ctrl progression share one coarse
  FreeRTOS mutex. 0x0501 returns notConfigured until approved timing is supplied;
  0x0502/0x0505 return notReady while RK safe-shutdown confirmation is missing.
  MCU-side DDR-ready enforcement remains pending handshake integration.
- 0x0006 is the generic command execution result, not an error-only response.
  It uses decimal `requestCommandId` and `status=accepted|completed|failed`;
  `errorCode` exists only for failed results and may be
  `invalidArgument|invalidDirection|notSupported|invalidState|busy|notReady|notConfigured|executionFailed`.
  非法头前缀逐字节丢弃，累计超过4096字节垃圾或半帧空闲500ms才断连；
  完整坏UTF-8/JSON回0006 invalidArgument后继续，不在正文内搜索控制帧。
  JSON词法检查拒绝原始NUL、未转义控制字符及`\u0000`，防止C字符串截断。
- B1 and V2 both keep D-Cache disabled. ETH DMA memory belongs in D2 SRAM and
  the FreeRTOS heap belongs in AXI SRAM; never add SCB cache maintenance calls.
- Result LED business states are ported to PG0-PG5. The 2026-08-27 BRD uses three
  L-C295JFLBCT amber/blue parts; the user accepted any bicolor combination on
  2026-09-01. Odd channels LED1/3/5 are amber FAIL/warning, even channels
  LED2/4/6 are blue PASS, both channels blink for normal testing, and amber blinks
  for hot testing. LPDDR0/LPDDR1/eMCP physical order still needs visual confirmation.
  PC uses target `lpddr0|lpddr1|emcp` and state
  `off|pass|fail|testNormal|testHot`; set/query/process share a short RTOS critical
  section. Keep PG6 off until its single-color meaning is frozen.
- Do not copy V2 pin mappings, PCF8574 PHY reset, fixed MAC or test scripts.
- `B1_RK_UART=4` is a software default, not a hardware decision; CMake value 2
  selects USART2. Register only one candidate as logical slot0, never USART1.
- The 2026-08-27 BRD confirms UART4 PA11/PA12 through U4008 to RK UART0_M0 and
  USART2 PD5/PD6 through U4013 to RK UART2_M0_DEBUG. Both electrical paths exist;
  this still does not decide which RK port is free for the production gateway.
- UART RX uses NORMAL DMA1 Stream0/UART4_RX and Stream1/USART2_RX with IRQ
  priority 5. HT is disabled/ignored; IDLE/TC blocks feed the RK parser. Register
  the queue before scheduling, but call uart_start_all() after scheduling starts.
- Keep `.uart_dma` in AXI SRAM after the RTOS heap. Reserve the fixed 30KiB LwIP
  allocator plus metadata in `.lwip_heap` at 0x30005000 (32KiB reserved), so ETH
  pools cannot silently overlap it. Never put UART DMA in DTCM.
- Preserve the default-OFF gates: RK commands/heartbeats and sensor sampling do
  not start before pwr_ctrl reaches ON. This does not implement safe-shutdown
  handshakes or flushing already queued UART TX bytes; those remain pending.
- B1 result LEDs are not TCP/RK debug indicators; never borrow PG0-PG6 for V2
  connection/heartbeat toggles. Exclude the old TLV GPIO reboot dispatcher.
- See docs/integration_20260831.md for migrated scope and remaining acceptance.
- The 2026-08-21 hardware review screenshot remains unconfirmed by request:
  PB3/PB4/PB5 presence detection is not an approved slot-ID interface. Do not
  configure these GPIOs, derive IP from them, or change the ETH1/PHY versus RMII
  switching path until the revised schematic and eight-slot level table are agreed.
  正式B1 IP已于2026-09-22确认为由B2 MCU告知，不以本地固定地址或FRU配置IP作为正式来源。
  当前测试继续使用本地配置192.168.1.201；本次仅记录，保留网络代码和FRU布局。
  B2侧分配方式、通知链路（是否SPI）/协议/时机、异常处理及是否持久化仍待双方确定，不擅自新增SPI命令。
- Build with `.\tools\dev.ps1 build`; run `.\tools\dev.ps1 check` before commit.
