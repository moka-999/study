# B1 MCU firmware

Independent STM32H743IIT6/LQFP176 project for the B1 memory-test board. It does
not import Apollo V2 application code until that module has passed V2 testing.

Current image validates the final B1 pin mux, 25 MHz HSE/400 MHz clock, safe
power defaults, YT8512H reset on PG12, W25Q256 QSPI pins, I2C1/2, the logical
I2C4 software bus on PH4/PH5, B1 SPI2 mode-0 hardware-NSS slave, AD7124 SPI4
mode-3 master and USART1/2/UART4. The V2-accepted result LED state machine has
been ported to PG0-PG5; PG6 remains off until its single-color status meaning is
frozen.
B1 uses a UID-derived locally administered MAC.
The V2-accepted W25Q256 driver and version/FRU v1 records are also present.
Startup initializes QSPI but deliberately does not write the production metadata
sectors. Each build emits `b1.bin`, `b1.manifest.json` and the 96-byte
`b1.version.bin` payload.

FreeRTOS 10.6.2 and LwIP 2.2.1 are now migrated as the final gateway runtime
baseline. The default task initializes static IPv4 `192.168.1.201/24`; ETH uses
the B1 YT8512H, PG12 reset and UID-derived MAC. D-Cache stays disabled on both
B1 and V2. The 128KiB RTOS heap is in AXI SRAM and all ETH DMA objects are in
D2 SRAM, so no cache clean/invalidate calls are permitted.

B1 has no temperature-control or forwarding role. PC controls the B2 MCU
directly; B1 SPI2 is reserved only for B1-B2 keepalive.

`b1.ioc` is the CubeMX pin/clock baseline and was generated with
`D:\stm32cubemx\STM32CubeMX.exe`. PH4/PH5 deliberately stay open-drain GPIOs:
the schematic calls this bus I2C4, but those pins are not an STM32 I2C4 AF pair.
SPI4 is stored in the `.ioc` and generated as AD7124 mode 3.

```powershell
.\tools\dev.ps1 check
.\tools\dev.ps1 build
```

Program `build_debug/boot.elf` first and `build_debug/b1.elf` second. Both V2
and B1 use the same bundled STM32Cube CMake, Ninja and GNU Arm toolchain.

## Migration rule

Move one module at a time from V2 only after its V2 build, module check and
logic-analyzer or hardware acceptance pass. Port behavior and protocol code;
keep pins, reset paths, PHY model, UART choice and peripheral handles local to
B1. Add only the smallest B1 check that proves the migrated module.

Not copied by design: V2 board glue, unaccepted `App`/`BSP` modules, board demos,
stress tests, extracted schematics and temporary files. The next milestone is
the TCP/TLV gateway task layer; PHY reset, MAC and RK UART routing remain B1-local.

## Result LED board check

The RTOS task advances the accepted non-blocking 500ms LED state machine. The
schematic only freezes LED1/2, LED3/4 and LED5/6 as physical pairs. Until the B1
board is visually checked, the board map provisionally treats odd LED signals as
red, even signals as green, and assigns the three pairs to LPDDR0, LPDDR1 and
eMCP in order.

On the first B1 board, verify PG0-PG5 with a short logic-analyzer power-on window
and visually confirm both color order and test-object order. Only
`s_result_led_map` in `Core/Src/main.c` should change after that check.
