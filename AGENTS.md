# B1 project rules

- Target: STM32H743IIT6, LQFP176, 25 MHz HSE, 400 MHz core.
- App starts at `0x08020000`; `boot.elf` starts at `0x08000000`.
- PHY is YT8512H; reset is PG12 `ETH_nRST`.
- PC6, PC7 and PG11 power enables must remain low until an explicit power-state
  machine enables them.
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
- `b1.ioc` is the CubeMX hardware baseline. After regeneration, restore the app
  linker origin `0x08020000`/length `1920K` and vector offset `0x020000`.
- Portable V2 modules may be integrated after their build/offline checks, as
  requested on 2026-08-31. Integration is not hardware acceptance: retain every
  unrun C test, network/UART check and B1 real-board calibration item explicitly.
- The V2-accepted AD7124-4 driver is ported and bound only to B1 SPI4. SPI4
  starts in mode 3 at about 390.625kHz; do not auto-run `ad7124_init()` before
  the first B1 board can provide a real ID and voltage-reference check.
- The V2-accepted MCP4725 driver is bound to B1 hardware I2C2 at address 0x60.
  It uses Fast Write only and must not write EEPROM. Do not set a Boot voltage
  automatically before B1 output-voltage and RK boot-mode validation.
- The V2-accepted TPS62868/PAC1934/DDR power baseline is ported without startup
  device access. VDD1/VDD2/VDDQ use I2C1/I2C2/PH4-PH5 software I2C respectively;
  `pwr_ctrl_init(NULL)` only reinforces PC6/PC7/PG11 low and leaves power-on
  prohibited until real-board timing is frozen. Runtime voltage offsets are
  independent signed 20mV steps across the TPS-register writable range; these
  extreme values are not DDR-safe and must never be swept automatically.
- W25Q256 now uses the 76-byte firmware payload and 224-byte FRU v2. Keep read
  compatibility for 96-byte firmware and 160/192-byte FRU records; preserve old
  identity/network fields and refuse to overwrite valid unsupported formats.
  Startup initializes QSPI but never auto-writes production metadata sectors.
  Runtime PC information comes from W25 loads, not firmware_version_current().
  Until Boot/factory writing exists, only a query may initialize blank/corrupt
  FRU or migrate a supported old record, followed by a verified readback.
- FreeRTOS 10.6.2 and LwIP 2.2.1 have been migrated for the final gateway using
  B1's existing YT8512 driver, UID-derived MAC and PG12 PHY reset. Formal TCP,
  RK, heartbeat and power-gated monitor tasks are integrated; never copy V2 board glue.
- The PC state-handler table and command-handler table are attached to the TCP
  server, using cJSON v1.7.19. The wire header is 18-byte big-endian
  plus a UTF-8 JSON object; the u32 Length field has a confirmed 4096-byte MCU
  PDU limit. cJSON uses pvPortMalloc/vPortFree and nesting depth 16. Current code
  still echoes request Seq. The 2026-08-31 target instead uses independent
  per-connection u32 send counters on PC and MCU, with ServiceID fixed to zero;
  this migration remains pending and must not change the independent RK Seq.
- Tester query 0x0101/0x0102 and empty-object heartbeat are integrated. The
  typed JSON helper covers START_TEST 0x0103
  (`algorithmName/algorithmIndex`) and TEST_RESULT_REPORT 0x0105
  (`testResults[{siteId,mirrorIndex,testStatus}]`), but algorithm-to-RK dispatch
  and result forwarding remain unimplemented. Business correlation and field
  semantics remain pending; do not invent their ranges.
- PC heartbeat is frozen as client request 0x0002 with `{}` and server response
  0x0001 with `{}`. Current migrated code still echoes PC Seq; the target is
  independent send counters, as stated above. Do not mark that target implemented.
- B1 and V2 both keep D-Cache disabled. ETH DMA memory belongs in D2 SRAM and
  the FreeRTOS heap belongs in AXI SRAM; never add SCB cache maintenance calls.
- Result LED color semantics have passed Apollo V2 and are now ported to PG0-PG5.
  The provisional board map is LED1/2, LED3/4, LED5/6 with odd=red/even=green
  and LPDDR0/LPDDR1/eMCP order. Change only `s_result_led_map` after the first B1
  visual check; keep PG6 off until the single-color status meaning is frozen.
- Do not copy V2 pin mappings, PCF8574 PHY reset, fixed MAC or test scripts.
- `B1_RK_UART=4` is a software default, not a hardware decision; CMake value 2
  selects USART2. Register only one candidate as logical slot0, never USART1.
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
  Static/DHCP/slot-based addressing and multi-B2 address allocation remain deferred.
- Build with `.\tools\dev.ps1 build`; run `.\tools\dev.ps1 check` before commit.
