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
- Do not copy an Apollo V2 module until its V2 acceptance check passes.
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
- The accepted W25Q256 driver and version/FRU v1 layout are ported. B1 boot only
  initializes QSPI; it must never auto-write the production metadata sectors.
- FreeRTOS 10.6.2 and LwIP 2.2.1 have been migrated for the final gateway using
  B1's YT8512H, UID-derived MAC and PG12 PHY reset. The next migration is the
  TCP dual-protocol task layer with UART4/USART2 RK routing; never copy V2 board glue.
- The V2-accepted PC transport parser and cJSON v1.7.19 are compiled in B1 but
  not yet attached to a TCP business task. The wire header is 18-byte big-endian
  plus a UTF-8 JSON object; the u32 Length field has a confirmed 4096-byte MCU
  PDU limit. cJSON uses pvPortMalloc/vPortFree and nesting depth 16. PC responses
  must echo the request Seq exactly; this u32 namespace is independent of RK Seq.
- The first typed JSON helper covers START_TEST 0x0103
  (`algorithmName/algorithmIndex`) and TEST_RESULT_REPORT 0x0105
  (`testResults[{siteId,mirrorIndex,testStatus}]`). ServiceID and field semantics
  remain pending; do not invent their ranges.
- PC heartbeat is frozen as client request 0x0002 with `{}` and server response
  0x0001 with `{}` using the same PC Seq. B1 only carries the command contract
  until its TCP task layer is migrated.
- B1 and V2 both keep D-Cache disabled. ETH DMA memory belongs in D2 SRAM and
  the FreeRTOS heap belongs in AXI SRAM; never add SCB cache maintenance calls.
- Result LED color semantics have passed Apollo V2 and are now ported to PG0-PG5.
  The provisional board map is LED1/2, LED3/4, LED5/6 with odd=red/even=green
  and LPDDR0/LPDDR1/eMCP order. Change only `s_result_led_map` after the first B1
  visual check; keep PG6 off until the single-color status meaning is frozen.
- Do not copy V2 pin mappings, PCF8574 PHY reset, fixed MAC or test scripts.
- Build with `.\tools\dev.ps1 build`; run `.\tools\dev.ps1 check` before commit.
