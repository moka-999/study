#!/usr/bin/env python3
"""Small static contract for the independent B1 bring-up project."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "Core/Inc/main.h"

EXPECTED = {
    "RMII_REF_CLK": "PA1", "ETH_MDIO": "PA2", "RMII_CRS_DV": "PA7",
    "ETH_MDC": "PC1", "RMII_RXD0": "PC4", "RMII_RXD1": "PC5",
    "RMII_TX_EN": "PB11", "RMII_TXD0": "PG13", "RMII_TXD1": "PG14",
    "ETH_NRST": "PG12",
    "QSPI_CLK": "PB2", "QSPI_NCS": "PB6", "QSPI_IO3": "PF6",
    "QSPI_IO2": "PF7", "QSPI_IO0": "PF8", "QSPI_IO1": "PF9",
    "I2C1_SDA": "PB7", "I2C1_SCL": "PB8", "I2C2_SDA": "PF0",
    "I2C2_SCL": "PF1", "I2C4_SCL": "PH4", "I2C4_SDA": "PH5",
    "SPI2_NCS": "PB12", "SPI2_SCK": "PB13", "SPI2_MISO": "PB14",
    "SPI2_MOSI": "PB15", "SPI4_NCS": "PE11", "SPI4_SCK": "PE12",
    "SPI4_MISO": "PE13", "SPI4_MOSI": "PE14",
    "USART1_TX": "PA9", "USART1_RX": "PA10", "UART4_RX": "PA11",
    "UART4_TX": "PA12", "USART2_TX": "PD5", "USART2_RX": "PD6",
    "PWR_4V0_EN": "PC6", "PWR_3V3_EN": "PC7", "PWR_DDR_EN": "PG11",
    "LED1": "PG0", "LED2": "PG1", "LED3": "PG2", "LED4": "PG3",
    "LED5": "PG4", "LED6": "PG5", "LED7": "PG6",
}


def require(ok: bool, message: str) -> None:
    if not ok:
        raise AssertionError(message)


def main() -> int:
    text = HEADER.read_text(encoding="utf-8")
    ports = dict(re.findall(r"#define B1_(\w+)_PORT\s+GPIO([A-I])", text))
    pins = {name: int(pin) for name, pin in re.findall(r"#define B1_(\w+)_PIN\s+GPIO_PIN_(\d+)", text)}
    actual = {name: f"P{ports[name]}{pin}" for name, pin in pins.items() if name in ports}
    require(actual == EXPECTED, f"pin contract mismatch\nexpected={EXPECTED}\nactual={actual}")
    require(len(set(actual.values())) == len(actual), "two signals share one pin")

    source = (ROOT / "Core/Src/main.c").read_text(encoding="utf-8")
    require("B1_PWR_4V0_EN_PIN | B1_PWR_3V3_EN_PIN, GPIO_PIN_RESET" in source,
            "PC6/PC7 must default low")
    require("B1_PWR_DDR_EN_PIN | B1_ETH_NRST_PIN" in source,
            "PG11/PG12 safe defaults missing")
    require("SPI_POLARITY_HIGH, SPI_PHASE_2EDGE" in source,
            "AD7124 SPI4 must use mode 3")
    require("SPI_MODE_SLAVE, SPI_NSS_HARD_INPUT" in source,
            "B1 SPI2 must be a hardware-NSS slave")
    require("B1_SPI2_NCS_PIN | B1_SPI2_SCK_PIN" in source and
            "HAL_GPIO_WritePin(B1_SPI2_NCS_PORT" not in source,
            "PB12 must be SPI2 NSS input, not a GPIO output")
    require("GPIO_MODE_OUTPUT_OD" in source and "GPIO_AF4_I2C4" not in source,
            "PH4/PH5 logical I2C4 must use software open-drain GPIO")

    ioc = (ROOT / "b1.ioc").read_text(encoding="utf-8")
    require("SPI4.CLKPolarity=SPI_POLARITY_HIGH" in ioc and
            "SPI4.CLKPhase=SPI_PHASE_2EDGE" in ioc,
            "CubeMX baseline must generate SPI4 mode 3")
    require("SPI2.Mode=SPI_MODE_SLAVE" in ioc and
            "SPI2.VirtualNSS=VM_NSSHARD" in ioc and
            "PB12.Mode=NSS_Signal_Hard_Input" in ioc,
            "CubeMX baseline must generate SPI2 slave with hardware NSS")
    require(all(f"PB{pin}.Mode=Full_Duplex_Slave" in ioc for pin in (13, 14, 15)) and
            "SPI2.CLKPolarity=SPI_POLARITY_LOW" in ioc and
            "SPI2.CLKPhase=SPI_PHASE_1EDGE" in ioc,
            "CubeMX baseline must keep SPI2 mode 0 slave pins")
    require("PH4.GPIO_Label=I2C4_SCL" in ioc and "PH5.GPIO_Label=I2C4_SDA" in ioc,
            "CubeMX baseline must preserve the PH4/PH5 logical I2C4 bus")

    board = (ROOT / "Core/Src/board_identity.c").read_text(encoding="utf-8")
    require("BOARD_PHY_YT8512H" in board and "mac[0] = 0x02" in board,
            "B1 identity must select YT8512H and a local unicast MAC")
    require(not (ROOT / "LWIP").exists() and not (ROOT / "Middlewares").exists(),
            "V2 middleware was copied before migration")
    print(f"B1 independent project: {len(actual)}/{len(EXPECTED)} pins PASS")
    print("Safe defaults, SPI2 slave mode 0, SPI4 mode 3, YT8512H and boundary PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
