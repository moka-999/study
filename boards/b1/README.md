# B1 final-pin bring-up project

`b1_pincheck.elf` is a standalone STM32H743IIT6/LQFP176 hardware-adaptation image. It shares the repository HAL driver objects but does not include FreeRTOS, LwIP, TLV, or application tasks.

## What it verifies

- B1 final pin mux: RMII, QSPI, I2C1/2/4, SPI2/4, USART1/2, UART4.
- 25 MHz HSE starts the MCU at 400 MHz; oscillator failure stops in `Error_Handler`.
- Safe GPIO defaults: PC6/PC7/PG11 stay low; SPI chip selects stay high.
- PHY reset is PG12 (`ETH_nRST`), not PA0.
- PG0-PG6 walk one LED every 500 ms for logic-analyzer/visual validation.
- USART1 sends `B1 final-pin check ready` at 115200 8N1.
- B1 MAC is stable per MCU UID and uses a locally administered unicast address. Apollo V2 keeps `00:80:E1:00:00:00`.

Build from the repository root:

```powershell
.\tools\dev.ps1 build
.\tools\dev.ps1 b1-check
```

The image links at `0x08020000`, like the main application, so program `boot.elf` first. SPI2 mode/rate are bring-up defaults only; freeze them with the B2 MCU owner before exchanging commands. The result-LED physical object order and the two product MAC allocation/FRU rules are still product inputs, not pin-mux inputs.
