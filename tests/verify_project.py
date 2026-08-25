#!/usr/bin/env python3
"""Small static contract for the independent B1 bring-up project."""

from pathlib import Path
import hashlib
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


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


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
    require("SPI_BAUDRATEPRESCALER_256" in source,
            "AD7124 SPI4 must start at the conservative 390.625kHz rate")
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
            "SPI4.CLKPhase=SPI_PHASE_2EDGE" in ioc and
            "SPI4.BaudRatePrescaler=SPI_BAUDRATEPRESCALER_256" in ioc,
            "CubeMX baseline must generate conservative SPI4 mode 3")
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

    # AD7124只同步V2已验收的驱动与SPI4板级适配；真板前不得启动即访问器件。
    ad_header = (ROOT / "BSP/AD7124/ad7124.h").read_text(encoding="utf-8")
    ad_source = (ROOT / "BSP/AD7124/ad7124.c").read_text(encoding="utf-8")
    spi_source = (ROOT / "HAL_Drv/spi_hw.c").read_text(encoding="utf-8")
    require(all(value in ad_header for value in (
        "AD7124_ID_AD7124_4_STD", "AD7124_ID_AD7124_4_B",
        "AD7124_ID_AD7124_4_REV_E", "AD7124_DIV_100R_10K")),
            "AD7124-4 ID白名单或B1分压系数缺失")
    require("AD7124_AIN_AIN0, AD7124_AIN_AIN2, AD7124_AIN_AIN4" in ad_source and
            "AD7124_AIN_AVSS" in ad_source,
            "AD7124 B1三路电压通道映射缺失")
    require("spi_if_t spi4_if" in spi_source and "&hspi4" in spi_source and
            "hspi2" not in spi_source,
            "AD7124必须只绑定B1 SPI4，不能占用SPI2保活总线")
    require("ad7124_init" not in source,
            "B1真板前不得在启动流程中自动访问AD7124")

    # MCP4725只绑定硬件I2C2，Fast Write不写EEPROM，启动期不改Boot模式。
    mcp_header = (ROOT / "BSP/MCP4725/mcp4725.h").read_text(encoding="utf-8")
    mcp_source = (ROOT / "BSP/MCP4725/mcp4725.c").read_text(encoding="utf-8")
    i2c_source = (ROOT / "HAL_Drv/i2c_hw.c").read_text(encoding="utf-8")
    require(all(value in mcp_header for value in (
        "MCP4725_ADDR_A0_LOW  0x60U", "BOOT_MODE_USB_MASKROM",
        "BOOT_MODE_EMMC_SD", "mcp4725_set_boot_mode")),
            "MCP4725地址、Boot模式或API缺失")
    require("0U, 300U, 600U, 900U, 1200U, 1500U, 1800U" in mcp_source and
            "MCP4725_POWER_NORMAL" in mcp_source,
            "RK Boot电压表或Fast Write正常模式缺失")
    require("i2c_if_t i2c2_if" in i2c_source and "&hi2c2" in i2c_source and
            "HAL_I2C_Master_Transmit" in i2c_source,
            "MCP4725必须通过B1硬件I2C2发送原始Fast Write数据")
    require("mcp4725_set_boot_mode" not in source,
            "B1真板前不得在启动流程中自动改写Boot模式")

    # TPS/PAC/DDR电源基线只绑定B1实际总线，启动仍保持全关且不访问器件。
    tps_header = (ROOT / "BSP/TPS62868/tps62868.h").read_text(encoding="utf-8")
    pac_header = (ROOT / "BSP/PAC1934/pac1934.h").read_text(encoding="utf-8")
    ddr_header = (ROOT / "HAL_Drv/ddr_power.h").read_text(encoding="utf-8")
    ddr_source = (ROOT / "HAL_Drv/ddr_power.c").read_text(encoding="utf-8")
    power_source = (ROOT / "HAL_Drv/pwr_ctrl.c").read_text(encoding="utf-8")
    monitor_source = (ROOT / "HAL_Drv/memory_monitor.c").read_text(encoding="utf-8")
    require(all(value in tps_header for value in (
        "TPS62868_ADDR_VDD1      0x49", "TPS62868_ADDR_VDD2      0x41",
        "TPS62868_ADDR_VDDQ      0x4F", "TPS62868_FACTOR_2X0",
        "TPS62868_FACTOR_1X0")), "TPS三路地址或器件因子错误")
    require("PAC1934_ADDR_B1             0x2C" in pac_header and
            "PAC1934_RSENSE_B1_UOHM      10000U" in pac_header and
            "PAC1934_CH_MASK_B1_RAILS    0x07" in pac_header,
            "PAC1934地址、采样电阻或三通道配置错误")
    require("&i2c1_if" in ddr_source and "&i2c2_if" in ddr_source and
            "&i2c4_if" in ddr_source,
            "VDD1/VDD2/VDDQ未分别绑定I2C1/I2C2/逻辑I2C4")
    require("DDR_VOLTAGE_STEP_MV      20" in ddr_header and
            "DDR_VDD1_WRITABLE_MIN_MV 800" in ddr_header and
            "DDR_VDD1_WRITABLE_MAX_MV 3350" in ddr_header and
            "DDR_VDDX_WRITABLE_MIN_MV 400" in ddr_header and
            "DDR_VDDX_WRITABLE_MAX_MV 1675" in ddr_header and
            "steps[rail] * DDR_VOLTAGE_STEP_MV" in ddr_source,
            "DDR三路20mV全可写范围接口缺失")
    require("B1_PWR_4V0_EN_PORT" in power_source and
            "B1_PWR_3V3_EN_PORT" in power_source and
            "B1_PWR_DDR_EN_PORT" in power_source and
            "pwr_ctrl_init(NULL);" in source,
            "B1电源状态机未绑定PC6/PC7/PG11安全OFF基线")
    require("memory_monitor_configure" in monitor_source and "valid_mask" in monitor_source,
            "TPS/PAC/AD未组合为可部分有效的监测快照")
    require("ddr_power_init();" not in source and "memory_monitor_init" not in source and
            "pwr_ctrl_request_on" not in source,
            "B1启动期禁止自动访问采样器件或打开DDR电源")

    # PH4/PH5软件I2C必须同时支持TPS寄存器访问和PAC多字节/Send Byte操作。
    require("i2c_if_t i2c4_if" in i2c_source and
            "soft_i2c_read_bytes" in i2c_source and
            "soft_i2c_write_command" in i2c_source and
            "soft_i2c_is_ready" in i2c_source and
            "SOFT_I2C_STRETCH_LIMIT" in i2c_source and
            "pulse < 9U" in i2c_source,
            "PH4/PH5软件I2C的交易、超时或总线恢复不完整")

    # 穷举确认以标称值为零点的全部20mV档位均能被TPS精确编码。
    for nominal, minimum, maximum, factor in (
        (1800, 800, 3350, 2), (1100, 400, 1675, 1),
        (1100, 400, 1675, 1), (600, 400, 1675, 1)):
        first = (minimum - nominal + 19) // 20
        last = (maximum - nominal) // 20
        for step in range(first, last + 1):
            target = nominal + step * 20
            base = target // factor
            require(target % factor == 0 and 400 <= base <= 1675 and
                    (base - 400) % 5 == 0,
                    f"20mV step cannot be encoded: nominal={nominal}, step={step}")

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
    require("BSP/AD7124/ad7124.c" in cmake and "HAL_Drv/spi_hw.c" in cmake,
            "B1 build does not compile the accepted AD7124 module")
    require("BSP/MCP4725/mcp4725.c" in cmake and "HAL_Drv/i2c_hw.c" in cmake,
            "B1 build does not compile the accepted MCP4725 module")
    require(all(path in cmake for path in (
        "BSP/TPS62868/tps62868.c", "BSP/PAC1934/pac1934.c",
        "HAL_Drv/ddr_power.c", "HAL_Drv/pwr_ctrl.c",
        "HAL_Drv/memory_monitor.c")),
            "B1 build does not compile the accepted power/monitor modules")

    # PC-MCU框架已在V2真实TCP上验收；B1此阶段只编译通用模块，不提前接业务任务。
    pc_header = (ROOT / "App/protocol/pc/pc_protocol.h").read_text(encoding="utf-8")
    pc_source = (ROOT / "App/protocol/pc/pc_protocol.c").read_text(encoding="utf-8")
    pc_test_source = (ROOT / "App/protocol/pc/pc_test_message.c").read_text(encoding="utf-8")
    pc_command = (ROOT / "App/protocol/pc/pc_command.h").read_text(encoding="utf-8")
    cjson_dir = ROOT / "Middlewares/Third_Party/cJSON"
    require(all(value in pc_header for value in (
        "PC_PROTOCOL_HEADER_SIZE   18U", "PC_PROTOCOL_VERSION       1U",
        "PC_PROTOCOL_FLAG_JSON     8U", "PC_PROTOCOL_MAX_PDU       4096U")),
            "B1 PC协议固定头或4KiB工程上限错误")
    require("cJSON_ParseWithLengthOpts" in pc_source and
            "PC_JSON_ERROR_DUPLICATE_KEY" in pc_source and
            "utf8_is_valid" in pc_source,
            "B1 PC协议严格JSON入口不完整")
    require("pc_json_configure_allocator(pvPortMalloc, vPortFree)" in rtos_source,
            "B1 cJSON未在任务创建前绑定FreeRTOS堆")
    require("App/protocol/pc/pc_protocol.c" in cmake and
            "App/protocol/pc/pc_test_message.c" in cmake and
            "Middlewares/Third_Party/cJSON/cJSON.c" in cmake and
            "CJSON_NESTING_LIMIT=16" in cmake,
            "B1构建未纳入PC协议/cJSON或深度限制")
    require(sha256(cjson_dir / "cJSON.c") ==
            "298581A04A36C0165DA4B0AADE235C23088CB2FAA58651D720EA2F3706ED0B0D" and
            sha256(cjson_dir / "cJSON.h") ==
            "25B0145150D500498E4D209CEC69C18C42CF818BFFCC54690BE3B895A2A16DEE",
            "B1 cJSON不是V2验收的v1.7.19固定源码")
    require(all(field in pc_test_source for field in (
        '"algorithmName"', '"algorithmIndex"', '"testResults"',
        '"siteId"', '"mirrorIndex"', '"testStatus"')),
            "B1未同步第一组算法/测试结果JSON结构")
    require("PC_COMMAND_SERVER_HEARTBEAT   0x0001U" in pc_command and
            "PC_COMMAND_CLIENT_HEARTBEAT   0x0002U" in pc_command,
            "B1 PC心跳命令合同未同步")

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
    print("AD7124-4 driver, SPI4-only binding and conservative mode-3 clock PASS")
    print("MCP4725 Boot map, Fast Write and I2C2-only binding contract PASS")
    print("TPS/PAC/AD monitor, pwr_ctrl and all 20mV DDR steps contract PASS")
    print("PC header, strict cJSON, algorithm/test-result JSON and allocator contract PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
