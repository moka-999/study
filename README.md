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

Not copied by design: V2 `App`, `BSP`, `LWIP`, `Middlewares`, board demos,
stress tests, extracted schematics and temporary files.

## Result LED bring-up image

The current bare-metal loop outputs the accepted sequence: all off 1s, LPDDR0
PASS 2s, LPDDR1 FAIL 2s, all three bicolor LEDs orange-blink for four 500ms/500ms
cycles, then red-blink for four cycles. The schematic only freezes LED1/2,
LED3/4 and LED5/6 as physical pairs. Until the B1 board is visually checked,
the board map provisionally treats odd LED signals as red, even signals as green,
and assigns the three pairs to LPDDR0, LPDDR1 and eMCP in order.

On the first B1 board, verify PG0-PG5 with a short logic-analyzer power-on window
and visually confirm both color order and test-object order. Only
`s_result_led_map` in `Core/Src/main.c` should change after that check.
