#!/usr/bin/env python3
"""B1板级安全及通用模块移植静态合同；不访问串口、网络或真实器件。"""

from pathlib import Path
import hashlib
import json
import re
import struct
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

    boot = (ROOT / "boot/boot.s").read_text(encoding="utf-8")
    boot_reset = boot.split("Reset_Handler:", 1)[1]
    require(all(token in boot for token in (
        "RCC_AHB4ENR,            0x580244E0",
        "GPIOC_BSRR,             0x58020818",
        "GPIOG_BSRR,             0x58021818",
        "GPIOC_EN_RESET,         0x00C00000",
        "GPIOG_EN_RESET,         0x08000000")),
        "Boot coupled-reset GPIO addresses or reset masks are incomplete")
    app_validation_index = boot_reset.index("ldr     r0, =APP_BASE")
    require(boot_reset.index("ldr     r0, =GPIOC_BSRR") <
            boot_reset.index("ldr     r0, =GPIOG_BSRR") < app_validation_index and
            boot_reset.index("ldr     r0, =GPIOC_MODER") <
            boot_reset.index("ldr     r0, =GPIOG_MODER") < app_validation_index,
            "Boot must force PC6/PC7/PG11 low before App validation")
    require("tst     r2, #1" in boot_reset and "beq     .Lfail" in boot_reset,
            "Boot must reject an even non-Thumb App reset vector")

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
    require("levels.pass_on = 1U" in led and "levels.fail_on = 1U" in led and
            "levels.pass_on = levels.fail_on" in led,
            "PASS/FAIL/normal-test channel conversion is incomplete")
    require("levels.fail_on ? GPIO_PIN_SET : GPIO_PIN_RESET" in led,
            "B1 2N7002 result LED outputs must be active high")
    require("result_led_get_all" in led and "result_led_get_all" in led_header and
            led.count("taskENTER_CRITICAL()") >= 4 and
            "s_states[target] == state" in led,
            "PC设置/查询结果灯与1ms闪烁推进必须共享原子状态保护")
    rtos_source = (ROOT / "Core/Src/freertos.c").read_text(encoding="utf-8")
    require("result_led_process(HAL_GetTick())" in rtos_source and "osDelay(1U)" in rtos_source,
            "result LED state machine must run without blocking the gateway")
    require(".fail_pin = B1_LED1_PIN" in source and ".pass_pin = B1_LED2_PIN" in source and
            ".fail_pin = B1_LED5_PIN" in source and ".pass_pin = B1_LED6_PIN" in source,
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
    require("PH4.GPIO_OutputLevel=GPIO_PIN_SET" in ioc and
            "PH5.GPIO_OutputLevel=GPIO_PIN_SET" in ioc,
            "CubeMX baseline must release both software-I2C lines high")
    require("Mcu.Pin48=PA13 (JTMS/SWDIO)" in ioc and
            "Mcu.Pin49=PA14 (JTCK/SWCLK)" in ioc and
            "PA13\\ (JTMS/SWDIO).Signal=SYS_JTMS-SWDIO" in ioc and
            "PA14\\ (JTCK/SWCLK).Signal=SYS_JTCK-SWCLK" in ioc,
            "CubeMX baseline must reserve PA13/PA14 for SWD")

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
    require("sizeof(firmware_version_info_t) == 76U" in flash_header and
            "sizeof(fru_info_t) == 224U" in flash_header and
            re.search(r"#define\s+FRU_INFO_SCHEMA_VERSION\s+2U", flash_header),
            "B1必须使用76字节版本载荷和224字节FRU v2")
    require("fru_info_load_legacy_v1" in flash_header and
            "fru_info_load_legacy_v1" in flash_layout,
            "B1原有192字节FRU v1必须保留读取迁移入口")

    # AD7124只同步V2已验收的驱动与SPI4板级适配；真板前不得启动即访问器件。
    ad_header = (ROOT / "BSP/AD7124/ad7124.h").read_text(encoding="utf-8")
    ad_source = (ROOT / "BSP/AD7124/ad7124.c").read_text(encoding="utf-8")
    spi_source = (ROOT / "HAL_Drv/spi_hw.c").read_text(encoding="utf-8")
    require(all(value in ad_header for value in (
        "AD7124_ID_AD7124_4_STD", "AD7124_ID_AD7124_4_B",
        "AD7124_ID_AD7124_4_REV_E", "AD7124_DIV_100R_10K")),
            "AD7124-4 ID白名单或B1分压系数缺失")
    require("AD7124_AIN_AIN0, AD7124_AIN_AIN2, AD7124_AIN_AIN4" in ad_source and
            "AD7124_AIN_AIN1, AD7124_AIN_AIN3, AD7124_AIN_AIN5" in ad_source and
            "AD7124_CH_AINM(b1_ainm[ch])" in ad_source,
            "AD7124 B1三路原理图差分通道映射缺失")
    require("AD7124_CFG_REF_SEL(AD7124_REF_INT)" in ad_source and
            "AD7124_CFG_REF_SEL(AD7124_REF_EXT1)" not in ad_source and
            "AD7124_CFG_AIN_BUFP" in ad_source and
            "AD7124_CFG_AIN_BUFM) != IF_OK" not in ad_source and
            "FB2306为NC" in ad_source,
            "AD7124必须使用内部2.5V基准且关闭接地负端缓冲")
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

    # TPS/PAC/DDR电源基线只绑定B1实际总线；当前BRD由PC6统一控制4V和TPS。
    tps_header = (ROOT / "BSP/TPS62868/tps62868.h").read_text(encoding="utf-8")
    pac_header = (ROOT / "BSP/PAC1934/pac1934.h").read_text(encoding="utf-8")
    ddr_header = (ROOT / "HAL_Drv/ddr_power.h").read_text(encoding="utf-8")
    ddr_source = (ROOT / "HAL_Drv/ddr_power.c").read_text(encoding="utf-8")
    power_header = (ROOT / "HAL_Drv/pwr_ctrl.h").read_text(encoding="utf-8")
    power_source = (ROOT / "HAL_Drv/pwr_ctrl.c").read_text(encoding="utf-8")
    monitor_source = (ROOT / "HAL_Drv/memory_monitor.c").read_text(encoding="utf-8")
    require(all(value in tps_header for value in (
        "TPS62868_ADDR_VDD1      0x49", "TPS62868_ADDR_VDD2      0x41",
        "TPS62868_ADDR_VDDQ      0x4F", "TPS62868_FACTOR_2X0",
        "TPS62868_FACTOR_1X0")), "TPS三路地址或器件因子错误")
    require("PAC1934_ADDR_B1             0x16U" in pac_header and
            "总线上写地址字节0x2C、读地址字节0x2D" in pac_header and
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
    require("POWER_ENABLE_DDR" not in power_source and
            "second_to_tps_config_delay_ms" in power_header and
            "second_to_ddr_delay_ms" not in power_header and
            "PWR_CTRL_STATE_WAIT_DDR" not in power_header and
            "write_enable(second_system_rail(), GPIO_PIN_RESET)" in power_source,
            "当前BRD必须由PC6/PC7两级状态机控制，并按反序关闭系统轨")
    require("write_optional_tps_gate(GPIO_PIN_SET);" in power_source and
            power_source.index("write_enable(second_system_rail(), GPIO_PIN_SET);") <
            power_source.index("write_optional_tps_gate(GPIO_PIN_SET);") and
            power_source.count("write_optional_tps_gate(GPIO_PIN_RESET);") >= 2 and
            "PC6仍会在系统轨流程中可靠关断" in power_source,
            "PG11必须只作防御性TPS总闸，且不能替代PC6完成可靠关断")
    require("memory_monitor_configure" in monitor_source and "valid_mask" in monitor_source,
            "TPS/PAC/AD未组合为可部分有效的监测快照")
    require("* 101" in monitor_source and "(scaled + 50) / 100" in monitor_source,
            "B1 AD7124电压快照必须应用100R+10k前端的1.01换算")
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
    require(all(option not in cmake for option in (
                "--build-number", "--git-commit", "--git-dirty")) and
            re.search(r'--target\s+"?b1-h743', cmake),
            "B1 CMake必须使用精简76字节清单接口并保留b1-h743目标")
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

    # 移植通用PC解析/查表与RK收发；正式TCP尚不等于PC到RK测试业务已打通。
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
            "utf8_is_valid" in pc_source and
            "json_string_lexemes_are_safe" in pc_source,
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
    require("write_be16(&output[8], 0U);" in pc_source and
            "write_be32(&output[12], mcu_tx_sequence);" in pc_source and
            "request->sequence" not in pc_source,
            "PC发送头必须使用ServiceID=0和MCU独立Seq")

    pc_state = (ROOT / "App/protocol/pc/pc_state_machine.c").read_text(encoding="utf-8")
    pc_state_header = (ROOT / "App/protocol/pc/pc_state_machine.h").read_text(encoding="utf-8")
    pc_dispatch = (ROOT / "App/protocol/pc/pc_dispatch.c").read_text(encoding="utf-8")
    pc_info = (ROOT / "App/protocol/pc/pc_tester_info.c").read_text(encoding="utf-8")
    rk_frame = (ROOT / "App/protocol/rk/frame.c").read_text(encoding="utf-8")
    rk_state = (ROOT / "App/protocol/rk/state_machine.c").read_text(encoding="utf-8")
    tcp_task = (ROOT / "App/tasks/pc/tcp_task.c").read_text(encoding="utf-8")
    rk_task = (ROOT / "App/tasks/rk/rk_task.c").read_text(encoding="utf-8")
    hb_task = (ROOT / "App/tasks/rk/hb_task.c").read_text(encoding="utf-8")
    monitor_task = (ROOT / "App/tasks/common/monitor_task.c").read_text(encoding="utf-8")
    task_header = (ROOT / "App/tasks/app_tasks.h").read_text(encoding="utf-8")
    require("s_state_handlers" in pc_state and "PC_SM_STATE_COUNT" in pc_state_header and
            all(f"[{state}]" in pc_state for state in (
                "PC_SM_STATE_HEADER", "PC_SM_STATE_BODY",
                "PC_SM_STATE_FRAME_READY", "PC_SM_STATE_ERROR")) and
            "(unsigned int)sm->state >= PC_SM_STATE_COUNT" in pc_state,
            "PC四态必须查表分发，并在索引前拒绝越界状态")
    require("s_command_table" in pc_dispatch and
            "s_command_table[i].handler(request, mcu_tx_sequence" in pc_dispatch and
            "pc_dispatch_handle" in tcp_task and "pc_sm_feed" in tcp_task and
            "tlv_dispatch(" not in tcp_task and "tlv_sm_feed" not in tcp_task,
            "PC状态表/命令表必须分层，正式TCP不能重新解析或分发RK TLV")
    require("PC_COMMAND_QUERY_POWER_STATUS" in pc_dispatch and
            "handle_query_power_status" in pc_dispatch and
            '"powerState"' in pc_dispatch and
            "PC_COMMAND_POWER_STATUS_RESP" in pc_dispatch,
            "PC首批0x0503/0x0504只读电源状态业务未接入")
    require(all(symbol in pc_command for symbol in (
                "PC_COMMAND_SET_DDR_CONFIG      0x0509U",
                "PC_COMMAND_QUERY_POWER_MONITOR 0x0601U",
                "PC_COMMAND_POWER_MONITOR_RESP  0x0602U",
                "PC_COMMAND_RESET_RK            0x0505U",
                "PC_COMMAND_SET_BOOT_MODE       0x0506U",
                "PC_COMMAND_QUERY_BOOT_MODE     0x0507U",
                "PC_COMMAND_BOOT_MODE_RESP      0x0508U",
                "PC_COMMAND_SET_RESULT_INDICATOR    0x0701U",
                "PC_COMMAND_QUERY_INDICATOR_STATUS  0x0702U",
                "PC_COMMAND_INDICATOR_STATUS_RESP   0x0703U")) and
            all(handler in pc_dispatch for handler in (
                "handle_set_ddr_config", "handle_query_power_monitor",
                "handle_power_on", "handle_power_off", "handle_reset_rk",
                "handle_set_boot_mode", "handle_query_boot_mode",
                "handle_set_result_indicator", "handle_query_indicator_status")),
            "PC DDR配置、采样查询或结果灯业务未完整接入命令表")
    require("fru_info_load" in pc_info and "fru_info_load_legacy_v1" in pc_info and
            "board_mac_address()" in pc_info and "pc_tester_info_load" in pc_dispatch and
            "firmware_version_current(" not in pc_dispatch,
            "PC信息查询必须读取持久FRU，空白B1身份使用自身UID MAC")
    require("return tlv_unpack(" in rk_state and
            re.search(r"tlv_sm_get_frame\([^;]+\)\s*<\s*0", rk_task) and
            "frame_len < TLV_PAYLOAD_MIN" in rk_frame and
            "unit_count >= TLV_MAX_PER_FRAME" in rk_frame and
            "invalid_frame:" in rk_frame,
            "RK必须传播解包错误并保留短帧/数量/整帧原子拒绝保护")
    require(all("rk_link_next_sequence()" in task for task in (rk_task, hb_task)),
            "RK命令与心跳必须通过独立序号入口分配，不借用PC Seq")

    task_paths = (
        "App/tasks/pc/tcp_task.c", "App/tasks/rk/rk_task.c",
        "App/tasks/rk/hb_task.c", "App/tasks/rk/rk_link.c",
        "App/tasks/common/monitor_task.c",
        "App/protocol/pc/pc_dispatch.c", "App/protocol/pc/pc_state_machine.c",
        "App/protocol/pc/pc_tester_info.c", "App/protocol/rk/frame.c",
        "App/protocol/rk/state_machine.c", "App/protocol/rk/crc.c",
    )
    require(all(path in cmake for path in task_paths), "正式任务/协议源码未全部加入B1构建")
    require(all(name in rtos_source and name in task_header for name in (
        "StartTcpServerTask", "StartRkTask", "StartHbTask", "StartMonitorTask")),
            "B1正式任务未统一声明或未在RTOS初始化中接入")
    require(all(re.search(rf"{name}\s*=\s*xQueueCreate\(\s*8U?\s*,", rtos_source)
                for name in ("pcToRk_Queue", "rkToPc_Queue")) and
            "pdPASS" in rtos_source and "Error_Handler()" in rtos_source,
            "RK业务队列必须各8槽，任务创建失败必须显式处理")
    require("monitor_power_process(HAL_GetTick())" in rtos_source and
            "pwr_ctrl_process(now_ms)" in monitor_task and
            all("pwr_ctrl_get_state()" in task and "PWR_CTRL_STATE_ON" in task
                for task in (rk_task, hb_task, monitor_task)),
            "B1必须推进电源状态机，RK发送和采样均受ON状态门控")
    require(re.search(r"memory_monitor_configure\([^;]*&i2c4_if\s*,\s*&spi4_if\s*,"
                      r"\s*(?:GPIOE|B1_SPI4_NCS_PORT)\s*,"
                      r"\s*(?:GPIO_PIN_11|B1_SPI4_NCS_PIN)", monitor_task) and
            "spi2_if" not in monitor_task,
            "B1监测只能使用逻辑I2C4和SPI4/PE11，不能占用SPI2保活")
    require("memory_monitor_init" in monitor_task and
            monitor_task.index("PWR_CTRL_STATE_ON") < monitor_task.index("memory_monitor_init") and
            not re.search(r"\b(?:ddr_power_init|memory_monitor_init|pwr_ctrl_request_on|"
                          r"mcp4725_set_boot_mode|fru_info_save|firmware_version_save)\s*\(",
                          source + rtos_source),
            "主启动/默认任务不得绕过ON门控自动访问器件或写生产元数据")
    require("monitor_task_prepare" in monitor_task and
            "xSemaphoreCreateMutexStatic" in monitor_task and
            monitor_task.count("memory_io_take(") >= 6 and
            "xSemaphoreGive(s_memory_io_mutex)" in monitor_task and
            "monitor_apply_ddr_config" in monitor_task,
            "监测采样和PC三轨调压必须以同一完整事务互斥量串行执行")
    require("PWR_CTRL_ERR_RESOURCE_BUSY" in monitor_task and
            "ready强制门待双方业务握手冻结" in monitor_task and
            "pwr_ctrl_apply_ddr_steps(steps)" in monitor_task and
            '"busy"' in pc_dispatch,
            "调压资源占用语义、ON执行路径或RK ready待办缺失")
    require(tcp_task.count("pc_last_byte_tick = xTaskGetTickCount();") >= 2 and
            tcp_task.index("for (int i = 0; i < n; i++)") <
            tcp_task.rindex("pc_last_byte_tick = xTaskGetTickCount();"),
            "TCP必须按实际消费字节刷新半帧计时，不能计入MCU业务处理时间")
    formal_tasks = rtos_source + tcp_task + rk_task + hb_task + monitor_task
    require(not (ROOT / "App/tasks/test").exists() and
            "App/tasks/test/" not in cmake and
            not re.search(r"\b(?:StartDriverMockTestTask|StartTestTask|StartQspiTestTask|"
                          r"StartAd7124WaveTask|StartPwrWaveTask|StartResultLedWaveTask)\s*\(",
                          formal_tasks) and
            not re.search(r"\bled_(?:on|off|toggle)\s*\(", formal_tasks),
            "B1正式启动不得包含V2测试任务或挪用结果灯作为V2诊断灯")

    uart_router = (ROOT / "HAL_Drv/uart_router.c").read_text(encoding="utf-8")
    uart_hw = (ROOT / "HAL_Drv/uart_hw.c").read_text(encoding="utf-8")
    interfaces = (ROOT / "HAL_Drv/board_interfaces.h").read_text(encoding="utf-8")
    require("B1_RK_UART" in rtos_source and
            all(re.search(rf"uart_register\(\s*0U?\s*,\s*&{port}\s*\)", rtos_source)
                for port in ("uart4_if", "uart2_if")) and
            not re.search(r"uart_register\(\s*1U?\s*,|\buart1_if\b", rtos_source) and
            "uart_start_all()" in rtos_source,
            "B1仅选择UART4/USART2中的一路注册slot0，USART1不得进入RK路由")
    require("extern uart_if_t uart4_if;" in interfaces and
            "extern uart_if_t uart2_if;" in interfaces and "uart1_if" not in interfaces and
            "uart_tx_mutex_init();" in uart_router and "uart_tx_isr_attach();" in uart_router,
            "B1 UART实例必须归属板级接口，并在启动接收前建立TX锁和中断钩子")
    require('section(".uart_dma")' in uart_router and
            re.search(r"\.uart_dma\s*\(NOLOAD\)\s*:\s*\{[^}]*\}\s*>\s*RAM\b", linker) and
            ".lwip_heap" in linker and "0x30005000" in linker,
            "UART DMA必须放AXI RAM，且显式预留D2中的固定LwIP堆防重叠")
    require("DMA_REQUEST_UART4_RX" in source and "DMA_REQUEST_USART2_RX" in source and
            "DMA_NORMAL" in source and "HAL_UARTEx_ReceiveToIdle_DMA" in uart_hw and
            "DMA_IT_HT" in uart_hw and "HAL_UART_RXEVENT_HT" in uart_router and
            "HAL_DMA_IRQHandler" in interrupts and "HAL_UART_IRQHandler" in interrupts,
            "B1 UART RX必须使用NORMAL DMA+IDLE，并忽略HT重复前缀")
    require(not re.search(r"\bSCB_(?:Enable|Clean|Invalidate)DCache\w*\s*\(",
                          source + uart_hw + uart_router + formal_tasks),
            "B1保持D-Cache关闭，新增UART/任务不能加入cache维护调用")

    # OTA复用公共实现，但健康观察和维护门必须接入B1各任务且不改硬件归属。
    ota_app = (ROOT / "App/ota/ota_app.c").read_text(encoding="utf-8")
    pc_ota = (ROOT / "App/protocol/pc/pc_ota.c").read_text(encoding="utf-8")
    require("ENABLE_OTA_BOOT" in cmake and "OTA_RELOCATION_PROBE=1" in cmake and
            "896K" in linker and "g_pfnVectors" in
            (ROOT / "Core/Src/system_stm32h7xx.c").read_text(encoding="utf-8"),
            "OTA必须限制槽容量并按可修正向量符号设置VTOR")
    require("StartOtaWatchdogTask" in rtos_source and "IWDG1->KR = 0xAAAAU" in rtos_source and
            "ota_app_watchdog_tick()" in rtos_source and
            all(f"ota_app_progress(OTA_TASK_{name})" in text for name, text in
                (("DEFAULT", rtos_source), ("TCP", tcp_task), ("MONITOR", monitor_task),
                 ("RK", rk_task), ("HB", hb_task))) and
            rk_task.split("void StartRkTask", 1)[1].index("ota_app_progress(OTA_TASK_RK)") <
            rk_task.split("void StartRkTask", 1)[1].index("continue;"),
            "五任务必须参与健康观察，OFF不能导致RK健康缺报")
    require("pc_dispatch_poll();" in tcp_task and "pc_dispatch_disconnected();" in tcp_task and
            "select(listenfd + 1" in tcp_task and "SO_SNDTIMEO" in tcp_task and
            "PC_COMMAND_MCU_OTA_REQUEST, pc_ota_handle" in pc_dispatch and
            '"otaNotReady"' in pc_dispatch and '"notSupported"' in pc_ota,
            "OTA命令/有界等待/断线生命周期或普通镜像拒绝入口缺失")
    require("monitor_ota_enter" in monitor_task and "s_ota_maintenance" in monitor_task and
            "flash_transaction_prepare()" in rtos_source and
            "flash_transaction_begin(1000U)" in pc_info and
            "firmware_version_load(" in ota_app and "firmware_version_current(" not in ota_app,
            "OTA必须串行访问Flash并保留OFF维护门和W25信息来源")
    print("OTA slot limits, five-task health, B1 OFF gate and shared Flash transactions PASS")

    manifest_path = ROOT / "build_debug/b1.manifest.json"
    image_path = ROOT / "build_debug/b1.bin"
    payload_path = ROOT / "build_debug/b1.version.bin"
    if manifest_path.exists() and image_path.exists() and payload_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        image = image_path.read_bytes()
        require(manifest["image_length"] == len(image), "B1清单镜像长度错误")
        require(manifest["image_crc32"] == f"0x{zlib.crc32(image) & 0xFFFFFFFF:08X}",
                "B1清单镜像CRC32错误")
        payload = payload_path.read_bytes()
        require(len(payload) == 76 and manifest["version_payload_length"] == 76,
                "B1版本载荷必须为76字节；若仍为旧96字节，请重新build")
        fields = struct.unpack("<HHHHH2xII24s16s16s", payload)
        require(fields[:2] == (1, 76) and fields[5] == len(image) and
                fields[6] == zlib.crc32(image) & 0xFFFFFFFF and
                fields[9].rstrip(b"\0") == b"b1-h743" and manifest["target"] == "b1-h743",
                "版本二进制载荷的schema/长度/CRC32/B1目标与清单不一致")
    print(f"B1 independent project: {len(actual)}/{len(EXPECTED)} pins PASS")
    print("Coupled-reset Boot early-off and optional PG11 gate contract PASS")
    print("Safe defaults, SPI2 slave mode 0, SPI4 mode 3 and YT8512H PASS")
    print("Result LED states, 500ms scheduler, active-high PG0-PG5 map and boundary PASS")
    print("W25Q256 32MiB layout, 4-byte QSPI, version/FRU and manifest contract PASS")
    print("FreeRTOS 128KiB heap, LwIP IPv4, YT8512H reset and UID MAC contract PASS")
    print("AD7124-4 driver, SPI4-only binding and conservative mode-3 clock PASS")
    print("MCP4725 Boot map, Fast Write and I2C2-only binding contract PASS")
    print("TPS/PAC/AD monitor, pwr_ctrl and all 20mV DDR steps contract PASS")
    print("PC header, strict cJSON, algorithm/test-result JSON and allocator contract PASS")
    print("PC state/command tables, RK error propagation and formal task migration PASS")
    print("Power-gated SPI4 monitor/RK traffic, single RK UART and AXI DMA contract PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
