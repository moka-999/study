# B1 MCU firmware

Independent STM32H743IIT6/LQFP176 project for the B1 memory-test board. It does
not import Apollo V2 application code until that module has passed V2 testing.

Current image validates the final B1 pin mux, 25 MHz HSE/400 MHz clock, safe
power defaults, YT8512H reset on PG12, W25Q256 QSPI pins, I2C1/2, the logical
I2C4 software bus on PH4/PH5, SPI2/4, USART1/2, UART4 and the PG0-PG6 LED walk.
B1 uses a UID-derived locally administered MAC.

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
