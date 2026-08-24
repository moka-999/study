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
- Result LED color semantics have passed Apollo V2 and are now ported to PG0-PG5.
  The provisional board map is LED1/2, LED3/4, LED5/6 with odd=red/even=green
  and LPDDR0/LPDDR1/eMCP order. Change only `s_result_led_map` after the first B1
  visual check; keep PG6 off until the single-color status meaning is frozen.
- Do not copy V2 pin mappings, PCF8574 PHY reset, fixed MAC or test scripts.
- Build with `.\tools\dev.ps1 build`; run `.\tools\dev.ps1 check` before commit.
