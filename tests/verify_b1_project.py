#!/usr/bin/env python3
"""Static contract check for the standalone B1 final-pin target."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "boards/b1/Core/Inc/main.h"

EXPECTED = {
    "RMII_REF_CLK": "PA1", "ETH_MDIO": "PA2", "RMII_CRS_DV": "PA7",
    "ETH_MDC": "PC1", "RMII_RXD0": "PC4", "RMII_RXD1": "PC5",
    "RMII_TX_EN": "PB11", "RMII_TXD0": "PG13", "RMII_TXD1": "PG14",
    "ETH_NRST": "PG12",
    "QSPI_CLK": "PB2", "QSPI_NCS": "PB6", "QSPI_IO3": "PF6",
    "QSPI_IO2": "PF7", "QSPI_IO0": "PF8", "QSPI_IO1": "PF9",
    "I2C1_SDA": "PB7", "I2C1_SCL": "PB8",
    "I2C2_SDA": "PF0", "I2C2_SCL": "PF1",
    "I2C4_SCL": "PH4", "I2C4_SDA": "PH5",
    "SPI2_NCS": "PB12", "SPI2_SCK": "PB13", "SPI2_MISO": "PB14", "SPI2_MOSI": "PB15",
    "SPI4_NCS": "PE11", "SPI4_SCK": "PE12", "SPI4_MISO": "PE13", "SPI4_MOSI": "PE14",
    "USART1_TX": "PA9", "USART1_RX": "PA10",
    "UART4_RX": "PA11", "UART4_TX": "PA12",
    "USART2_TX": "PD5", "USART2_RX": "PD6",
    "PWR_4V0_EN": "PC6", "PWR_3V3_EN": "PC7", "PWR_DDR_EN": "PG11",
    "LED1": "PG0", "LED2": "PG1", "LED3": "PG2", "LED4": "PG3",
    "LED5": "PG4", "LED6": "PG5", "LED7": "PG6",
}


def parse_pins(text: str) -> dict[str, str]:
    ports = dict(re.findall(r"#define B1_(\w+)_PORT\s+GPIO([A-I])", text))
    pins = {name: int(pin) for name, pin in re.findall(r"#define B1_(\w+)_PIN\s+GPIO_PIN_(\d+)", text)}
    return {name: f"P{ports[name]}{pin}" for name, pin in pins.items() if name in ports}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    actual = parse_pins(HEADER.read_text(encoding="utf-8"))
    require(actual == EXPECTED, f"pin contract mismatch\nexpected={EXPECTED}\nactual={actual}")
    require(len(set(actual.values())) == len(actual), "two B1 signals share one MCU pin")
    require(actual["ETH_NRST"] == "PG12", "PHY reset must be PG12")

    source = (ROOT / "boards/b1/Core/Src/main.c").read_text(encoding="utf-8")
    require("B1_PWR_4V0_EN_PIN | B1_PWR_3V3_EN_PIN, GPIO_PIN_RESET" in source,
            "PC6/PC7 must default low")
    require("B1_PWR_DDR_EN_PIN | B1_ETH_NRST_PIN" in source, "PG11/PG12 safe defaults missing")
    require("SPI_POLARITY_HIGH, SPI_PHASE_2EDGE" in source, "AD7124 SPI4 must use mode 3")
    require("RCC_HSE_ON" in source and "osc.PLL.PLLM = 5" in source and "osc.PLL.PLLN = 160" in source,
            "B1 must validate the 25 MHz HSE at 400 MHz")

    identity = (ROOT / "HAL_Drv/board_identity.c").read_text(encoding="utf-8")
    require("0x00, 0x80, 0xE1, 0x00, 0x00, 0x00" in identity, "Apollo V2 MAC changed")
    require("mac[0] = 0x02" in identity and "HAL_GetUIDw0()" in identity,
            "B1 must use a UID-derived locally administered MAC")

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    require("add_executable(b1_pincheck" in cmake, "b1_pincheck target missing")
    print(f"B1 pin contract: {len(actual)}/{len(EXPECTED)} signals PASS")
    print("B1 safe defaults, SPI4 mode 3, and board-specific MAC policy PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
