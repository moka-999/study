#!/usr/bin/env python3
"""Small static contract for the independent B1 bring-up project."""

from pathlib import Path
import json
import re
import sys
import zlib

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

    led = (ROOT / "Core/Src/result_led.c").read_text(encoding="utf-8")
    led_header = (ROOT / "Core/Inc/result_led.h").read_text(encoding="utf-8")
    require("RESULT_LED_BLINK_MS   500U" in led_header,
            "result LED half-period must remain 500ms")
    require(all(name in led and name in led_header for name in (
        "RESULT_LED_PASS", "RESULT_LED_FAIL", "RESULT_LED_TEST_NORMAL",
        "RESULT_LED_TEST_HOT")), "result LED business states are incomplete")
    require("levels.green_on = 1U" in led and "levels.red_on = 1U" in led and
            "levels.green_on = levels.red_on" in led,
            "PASS/FAIL/orange color conversion is incomplete")
    require("levels.red_on ? GPIO_PIN_SET : GPIO_PIN_RESET" in led,
            "B1 2N7002 result LED outputs must be active high")
    rtos_source = (ROOT / "Core/Src/freertos.c").read_text(encoding="utf-8")
    require("result_led_process(HAL_GetTick())" in rtos_source and "osDelay(1U)" in rtos_source,
            "result LED state machine must run without blocking the gateway")
    require("B1_LED1_PIN, GPIOG, B1_LED2_PIN" in source and
            "B1_LED5_PIN, GPIOG, B1_LED6_PIN" in source,
            "three B1 bicolor LED pairs are not mapped")
    require("B1_LED7_PIN" not in source.split("s_result_led_map", 1)[1].split("};", 1)[0],
            "PG6 single-color status LED must not enter the result LED map")

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

    # B1最终网关必须保留V2已验收的RTOS/LwIP内核，但板级PHY、MAC和复位不能照搬。
    freertos_config = (ROOT / "Core/Inc/FreeRTOSConfig.h").read_text(encoding="utf-8")
    lwip_options = (ROOT / "LWIP/Target/lwipopts.h").read_text(encoding="utf-8")
    ethernet = (ROOT / "LWIP/Target/ethernetif.c").read_text(encoding="utf-8")
    ethernet_board = (ROOT / "Core/Src/ethernet_board.c").read_text(encoding="utf-8")
    interrupts = (ROOT / "Core/Src/stm32h7xx_it.c").read_text(encoding="utf-8")
    require("configTOTAL_HEAP_SIZE                    ((size_t)131072)" in freertos_config,
            "FreeRTOS heap must remain 128KiB")
    require("#define WITH_RTOS 1" in lwip_options,
            "LwIP must use its FreeRTOS sys_arch port")
    require("osKernelInitialize()" in source and "osKernelStart()" in source and
            "MX_LWIP_Init();" in rtos_source,
            "LwIP must initialize from a running RTOS task")
    require("board_mac_address()" in ethernet and "0x80" not in ethernet,
            "B1 Ethernet must use UID-derived MAC instead of the V2 fixed MAC")
    require("YT8512H" in ethernet and "YT8512C" not in ethernet and "PCF8574" not in ethernet,
            "B1 Ethernet glue contains V2 PHY/reset residue")
    require("CORTEX_M7.CPU_DCache=Disabled" in ioc and
            "SCB_InvalidateDCache" not in ethernet and "SCB_CleanDCache" not in ethernet,
            "B1 must keep D-Cache disabled without cache-maintenance calls")
    require("B1_ETH_NRST_PIN, GPIO_PIN_RESET" in source and
            source.index("B1_ETH_NRST_PIN, GPIO_PIN_SET") < source.index("osKernelInitialize()"),
            "PG12 must reset YT8512H before starting the scheduler")
    require("HAL_NVIC_SetPriority(ETH_IRQn, 5U, 0U)" in ethernet_board and
            "HAL_ETH_IRQHandler(&heth)" in interrupts,
            "ETH interrupt priority/handler is not FreeRTOS safe")

    # W25Q256模块必须保持V2已验收的四字节寻址、RMW和正式元数据地址。
    flash_header = (ROOT / "BSP/Flash/flash_layout.h").read_text(encoding="utf-8")
    flash_layout = (ROOT / "BSP/Flash/flash_layout.c").read_text(encoding="utf-8")
    flash_bsp = (ROOT / "BSP/Flash/flash_bsp.c").read_text(encoding="utf-8")
    qspi = (ROOT / "HAL_Drv/qspi_hw.c").read_text(encoding="utf-8")
    require(all(value in flash_header for value in (
        "0x00000000UL", "0x00001000UL", "0x00002000UL", "0x00003000UL",
        "0x01FF0000UL", "0x02000000UL")), "W25Q256分区地址不完整")
    require("ENABLE_FLASH_SELF_TEST" not in flash_header,
            "B1正式布局不得包含Apollo自检地址重映射")
    require("generation_is_newer" in flash_layout and "payload_crc32" in flash_layout,
            "版本/FRU双副本CRC选择逻辑缺失")
    require("memcmp(s_sector_buffer" in flash_bsp and "page_is_erased" in flash_bsp,
            "Flash扇区RMW或磨损规避逻辑缺失")
    require("QSPI_ADDRESS_32_BITS" in qspi and "W25Q_CAPACITY - addr" in qspi,
            "W25Q256四字节寻址或容量边界检查缺失")
    require("flash_if.init(&flash_if)" in source and
            "firmware_version_save" not in source and "fru_info_save" not in source,
            "B1启动应初始化Flash但不得自动写生产元数据")

    linker = (ROOT / "STM32H743xx_FLASH.ld").read_text(encoding="utf-8")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    require("KEEP(*(.firmware_version))" in linker,
            "编译期固件身份可能被链接器回收")
    require("gen_firmware_manifest.py" in cmake and "b1.version.bin" in cmake,
            "B1版本载荷构建步骤缺失")
    require(".heap (NOLOAD)" in linker and ">RAM" in linker and
            all(section in linker for section in (
                ".RxDescripSection", ".TxDescripSection", ".Rx_PoolSection", ">RAM_D2")),
            "FreeRTOS heap or Ethernet DMA memory placement is missing")
    require("Middlewares/Third_Party/FreeRTOS" in cmake and
            "Middlewares/Third_Party/LwIP" in cmake,
            "B1 build does not compile the migrated FreeRTOS/LwIP sources")

    manifest_path = ROOT / "build_debug/b1.manifest.json"
    image_path = ROOT / "build_debug/b1.bin"
    payload_path = ROOT / "build_debug/b1.version.bin"
    if manifest_path.exists() and image_path.exists() and payload_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        image = image_path.read_bytes()
        require(manifest["image_length"] == len(image), "B1清单镜像长度错误")
        require(manifest["image_crc32"] == f"0x{zlib.crc32(image) & 0xFFFFFFFF:08X}",
                "B1清单镜像CRC32错误")
        require(len(payload_path.read_bytes()) == 96, "B1版本载荷必须为96字节")
    print(f"B1 independent project: {len(actual)}/{len(EXPECTED)} pins PASS")
    print("Safe defaults, SPI2 slave mode 0, SPI4 mode 3 and YT8512H PASS")
    print("Result LED states, 500ms scheduler, active-high PG0-PG5 map and boundary PASS")
    print("W25Q256 32MiB layout, 4-byte QSPI, version/FRU and manifest contract PASS")
    print("FreeRTOS 128KiB heap, LwIP IPv4, YT8512H reset and UID MAC contract PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
