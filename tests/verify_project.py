#!/usr/bin/env python3
"""B1板级安全及通用模块移植静态合同；不访问串口、网络或真实器件。"""

from pathlib import Path
import hashlib
import argparse
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
    "SLOT_ID0": "PB3", "SLOT_ID1": "PB4", "SLOT_ID2": "PB5",
}


def require(ok: bool, message: str) -> None:
    if not ok:
        raise AssertionError(message)


def source_sha256(path: Path) -> str:
    # 固定C源码统一按LF核验，兼容Git检出/导出的CRLF；其它字节变化仍会拒绝。
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest().upper()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/standalone/Debug")
    args = parser.parse_args()
    # Ctrl+Shift+B用于只取ELF/BIN；显式烧录任务必须由用户另外选择。
    tasks = json.loads((ROOT / ".vscode/tasks.json").read_text(encoding="utf-8-sig"))["tasks"]
    defaults = [task for task in tasks if task.get("group") == {"kind": "build", "isDefault": True}]
    require(len(defaults) == 1 and defaults[0]["args"][-2:] == ["${workspaceFolder}/tools/dev.ps1", "build"],
            "B1默认构建任务必须只调用dev.ps1 build")
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

    startup = (ROOT / "startup_stm32h743xx.s").read_text(encoding="utf-8")
    reset = startup.split("Reset_Handler:", 1)[1]
    require(all(token in startup for token in (
        "RCC_AHB4ENR,            0x580244E0",
        "GPIOC_BSRR,             0x58020818",
        "GPIOG_BSRR,             0x58021818",
        "GPIOC_EN_RESET,         0x00C00000",
        "GPIOG_EN_RESET,         0x08000000")),
        "独立启动入口缺少耦合复位GPIO地址或低电平掩码")
    first_init = reset.index("bl  ExitRun0Mode")
    require(reset.index("ldr     r0, =GPIOC_BSRR") <
            reset.index("ldr     r0, =GPIOG_BSRR") <
            reset.index("ldr     r0, =GPIOC_MODER") <
            reset.index("ldr     r0, =GPIOG_MODER") < first_init <
            reset.index("bl  SystemInit") < reset.index("CopyDataInit:"),
            "必须先锁存低电平、配置PC6/PC7/PG11输出，再初始化系统和RAM")
    boot_startup = (ROOT / "boot/startup.S").read_text(encoding="utf-8")
    require(all(value in boot_startup for value in (
                "BOOT_BOARD_B1", "0x580244E0", "0x58020818", "0x58021818",
                "0x00C00000", "0x08000000")) and
            boot_startup.index("ldr r0, =0x58020818") < boot_startup.index("ldr r0, =_sidata"),
            "精简Boot也必须在初始化RAM前关闭PC6/PC7/PG11")

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
    default_task = rtos_source.split("static void StartDefaultTask", 1)[1].split("#if", 1)[0]
    ready_led = "result_led_mcu_ready(B1_LED7_PORT, B1_LED7_PIN);"
    require(default_task.index("MX_LWIP_Init();") < default_task.index(ready_led) <
            default_task.index("for (;;)") and "board_slot_code()" in default_task,
            "PG6应在默认任务初始化完成后常亮，并读取槽位码；不新增灯任务")
    require("B1_LED7_PIN, GPIO_PIN_RESET" in source and
            "B1_LED7_PIN" not in default_task.split("for (;;)", 1)[1],
            "PG6必须先灭后亮，不能在任务循环中闪烁或随测试结果变化")
    slot_init = source.split("gpio.Mode = GPIO_MODE_INPUT;", 1)[1].split(
        "HAL_GPIO_Init(B1_SLOT_ID0_PORT, &gpio);", 1)[0]
    require("gpio.Pull = GPIO_NOPULL;" in slot_init and
            "gpio.Pin = B1_SLOT_ID0_PIN | B1_SLOT_ID1_PIN | B1_SLOT_ID2_PIN;" in slot_init,
            "PB3～PB5必须为输入、无内部上下拉；由B1外部上拉及B2电阻编码")

    ioc = (ROOT / "b1.ioc").read_text(encoding="utf-8")
    for bit, pin in enumerate(("PB3\\ (JTDO/TRACESWO)", "PB4\\ (NJTRST)", "PB5")):
        require(all(f"{pin}.{setting}" in ioc for setting in (
            "Signal=GPIO_Input", "GPIO_PuPd=GPIO_NOPULL", f"GPIO_Label=SLOT_ID{bit}")),
            f"CubeMX必须保留槽位输入{pin}及其位序")
    require("PG6.GPIO_OutputLevel=GPIO_PIN_RESET" in ioc and
            "PG6.Signal=GPIO_Output" in ioc, "CubeMX必须保留PG6启动时灭灯")
    ioc_pins = re.findall(r"^Mcu.Pin\d+=(.+)$", ioc, flags=re.MULTILINE)
    require(len(ioc_pins) == int(re.search(r"Mcu.PinsNb=(\d+)", ioc).group(1)) ==
            len(set(ioc_pins)), "CubeMX引脚总数和唯一性必须一致")
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
    require("flash_if.init(&flash_if)" in source and "firmware_version_" not in source and
            "fru_info_save" not in source and source.index("flash_if.init(&flash_if)") <
            source.index("osKernelInitialize()"),
            "B1启动只初始化W25器件，不读写固件版本或重置FRU")
    require("IWDG_PRESCALER_256" in source and "HAL_IWDG_Init(&watchdog)" in source and
            source.index("flash_if.init(&flash_if)") < source.index("    MX_IWDG_Init();") <
            source.index("osKernelInitialize()"),
            "启用IWDG时须在外设初始化后、任务启动前启动")
    require("sizeof(firmware_version_info_t) == 76U" in flash_header and
            "sizeof(fru_info_t) == 224U" in flash_header and
            re.search(r"#define\s+FRU_INFO_SCHEMA_VERSION\s+2U", flash_header),
            "B1必须使用76字节版本载荷和224字节FRU v2")
    require("fru_info_load_legacy_v1" in flash_header and
            "fru_info_load_legacy_v1" in flash_layout,
            "B1原有192字节FRU v1必须保留读取迁移入口")

    # AD7124保持SPI4绑定，启动阶段不访问尚未供电的器件。
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
    adc_config = (ROOT / "Core/Inc/board_adc_config.h").read_text(encoding="utf-8")
    monitor_source = (ROOT / "HAL_Drv/memory_monitor.c").read_text(encoding="utf-8")
    require("use_avdd_reference" in ad_header and "AD7124_REF_AVDD" in ad_source and
            "write_error_enable" in ad_source,
            "AD7124基准选择或诊断回读入口缺失；具体寄存器和故障分支由实际C回归验证")
    require("BOARD_AD_USE_AVDD_REFERENCE" in adc_config and "BOARD_AD_REFERENCE_MV" in adc_config and
            "monitor->adc.use_avdd_reference = BOARD_AD_USE_AVDD_REFERENCE;" in monitor_source and
            monitor_source.count("(float)BOARD_AD_REFERENCE_MV / 1000.0f") == 2,
            "B1基准选择、同步采样和增量扫描必须共用板级基准配置")
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
            "PAC1934_RSENSE_B1_UOHM      20000U" in pac_header and
            "PAC1934_CH_MASK_B1_RAILS    0x07" in pac_header,
            "PAC1934地址、采样电阻或三通道配置错误")
    require("monitor->pac.rsense_uohm = PAC1934_RSENSE_B1_UOHM;" in monitor_source,
            "B1采样必须使用2134版20mΩ板级值，不能落到通用10mΩ兼容值")
    require("&i2c1_if" in ddr_source and "&i2c2_if" in ddr_source and
            "&i2c4_if" in ddr_source,
            "VDD1/VDD2/VDDQ未分别绑定I2C1/I2C2/逻辑I2C4")
    require("DDR_VDD1_STEP_MV         10" in ddr_header and
            "DDR_VDDX_STEP_MV         5" in ddr_header and
            "DDR_VDD1_WRITABLE_MIN_MV 800" in ddr_header and
            "DDR_VDD1_WRITABLE_MAX_MV 3350" in ddr_header and
            "DDR_VDDX_WRITABLE_MIN_MV 400" in ddr_header and
            "DDR_VDDX_WRITABLE_MAX_MV 1675" in ddr_header and
            "steps[rail] * _step_mv[rail]" in ddr_source,
            "DDR三路原生步进全可写范围接口缺失")
    require("B1_PWR_4V0_EN_PORT" in power_source and
            "B1_PWR_3V3_EN_PORT" in power_source and
            "B1_PWR_DDR_EN_PORT" in power_source and
            '#include "board_power_config.h"' in source and
            "pwr_ctrl_init(&b1_power_default_timing);" in source and
            "pc_power_service_configure(&b1_power_default_business, &s_power_rk)" in source and
            ".begin = rk_link_ready_begin" in source and ".poll = rk_link_ready_poll" in source,
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

    # 穷举确认以标称值为零点的原生档位覆盖每轨全部256个TPS码。
    for nominal, minimum, maximum, factor in (
        (1800, 800, 3350, 2), (1100, 400, 1675, 1),
        (1100, 400, 1675, 1), (600, 400, 1675, 1)):
        step_mv = factor * 5
        first = (minimum - nominal) // step_mv
        last = (maximum - nominal) // step_mv
        require(last - first + 1 == 256, "每轨应有256个原生档位")
        for step in range(first, last + 1):
            target = nominal + step * step_mv
            base = target // factor
            require(target % factor == 0 and 400 <= base <= 1675 and
                    (base - 400) % 5 == 0,
                    f"原生档位不能精确编码: nominal={nominal}, step={step}")

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

    # 通用PC解析/查表与真实RK业务共用；静态检查不能替代B1真板验收。
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
    require(source_sha256(cjson_dir / "cJSON.c") ==
            "298581A04A36C0165DA4B0AADE235C23088CB2FAA58651D720EA2F3706ED0B0D" and
            source_sha256(cjson_dir / "cJSON.h") ==
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
                "PC_COMMAND_ROUND_BEGIN",
                "PC_COMMAND_ROUND_END",
                "PC_COMMAND_SLT_START", "PC_COMMAND_SLT_RESULT")) and
            all(handler in pc_dispatch for handler in (
                "handle_set_ddr_config", "handle_query_power_monitor",
                "handle_power_on", "handle_power_off", "handle_reset_rk",
                "handle_set_boot_mode", "handle_query_boot_mode",
                "handle_round_begin", "handle_round_end", "handle_slt_start")),
            "PC DDR配置、采样查询或结果灯业务未完整接入命令表")
    require("handle_slt_result" not in pc_dispatch and
            not re.search(r"\{\s*PC_COMMAND_SLT_RESULT\s*,", pc_dispatch),
            "历史010B只保留编号，不能继续参与SLT执行收尾")
    require(not re.search(r"0x070[123]U", pc_command) and
            "handle_set_result_indicator" not in pc_dispatch and
            "handle_query_indicator_status" not in pc_dispatch,
            "070x旧结果灯命令必须从固件删除")
    boot_fru = (ROOT / "boot/boot_fru.c").read_text(encoding="utf-8")
    boot_main = (ROOT / "boot/boot_main.c").read_text(encoding="utf-8")
    require("fru_info_load" in pc_info and "fru_info_save" not in pc_info and
            "#if !BOARD_W25_PRESENT" in pc_info and
            "fru_info_load_legacy_v1" in boot_fru and "fru_info_save" in boot_fru and
            "UID_BASE" in boot_main and "pc_tester_info_load" in pc_dispatch and
            "firmware_version_current(" not in pc_dispatch,
            "Boot维护持久FRU，App只读；未焊W25首板显式使用常量")
    require("return tlv_unpack(" in rk_state and
            re.search(r"tlv_sm_get_frame\([^;]+\)\s*<\s*0", rk_task) and
            "length < TLV_PAYLOAD_MIN" in rk_frame and
            "frame->count >= TLV_MAX_PER_FRAME" in rk_frame and
            "invalid_frame:" in rk_frame,
            "RK必须传播解包错误并保留短帧/数量/整帧原子拒绝保护")
    require(rk_task.count("rk_link_next_sequence()") == 3 and
            "tlv_pack_frequency(value, rk_link_next_sequence()" in rk_task and
            "process_heartbeat(&last_heartbeat);" in rk_task,
            "RK命令、心跳与启动频率回复必须通过独立序号入口分配，不借用PC Seq")
    require(not (ROOT / "App/tasks/rk/hb_task.c").exists() and
            not (ROOT / "App/tasks/rk/hb_task.h").exists() and
            "StartHbTask" not in rtos_source + task_header and "hb_task.c" not in cmake,
            "心跳必须合入RK任务，不残留独立HB线程或构建项")

    task_paths = (
        "App/tasks/pc/tcp_task.c", "App/tasks/rk/rk_task.c",
        "App/tasks/rk/rk_link.c",
        "App/tasks/common/monitor_task.c",
        "App/protocol/pc/pc_dispatch.c", "App/protocol/pc/pc_state_machine.c",
        "App/protocol/pc/pc_tester_info.c", "App/protocol/pc/pc_test_service.c",
        "App/protocol/rk/frame.c",
        "App/protocol/rk/state_machine.c", "App/protocol/common/crc.c",
    )
    require(all(path in cmake for path in task_paths), "正式任务/协议源码未全部加入B1构建")
    pc_service = (ROOT / "App/protocol/pc/pc_test_service.c").read_text(encoding="utf-8")
    require("pc_dispatch_next_response" in tcp_task and
            all(token in pc_service for token in (
                "!start->external_ready", "pwr_ctrl_get_state() != PWR_CTRL_STATE_ON",
                "monitor_idd_start", "monitor_idd_finish", "pc_test_add_measurement")) and
            "pc_test_add_statistics" in pc_dispatch,
            "B1真实算法必须受ON门控，统计及异步结果都须接入正式TCP")
    require("if(ENABLE_V2_POWER_SIMULATION OR ENABLE_RK_GATEWAY_TEST OR ENABLE_V2_MATS_FAIL_TEST)" in cmake and
            "FATAL_ERROR" in cmake and "ENABLE_V2_MATS_FAIL_TEST" not in pc_service and
            '"SAF"' not in pc_service,
            "B1必须拒绝V2模拟开关，且正式结果服务不包含MATS假FAIL/SAF注入")
    require(all(name in rtos_source and name in task_header for name in (
        "StartTcpServerTask", "StartRkTask", "StartMonitorTask")),
            "B1正式任务未统一声明或未在RTOS初始化中接入")
    require(all(re.search(rf"{name}\s*=\s*xQueueCreate\(\s*8U?\s*,", rtos_source)
                for name in ("rkRequestQueue", "rkResponseQueue")) and
            "pdPASS" in rtos_source and "Error_Handler()" in rtos_source,
            "RK业务队列必须各8槽，任务创建失败必须显式处理")
    require("monitor_power_process(HAL_GetTick())" in rtos_source and
            "pwr_ctrl_process(now_ms)" in monitor_task and
            all("pwr_ctrl_get_state()" in task and "PWR_CTRL_STATE_ON" in task
                for task in (rk_task, monitor_task)),
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
    formal_tasks = rtos_source + tcp_task + rk_task + monitor_task
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
    require(all(f"uart_register(UART_PORT_{slot}, &{port})" in rtos_source
                for slot, port in (("RK_PROTOCOL", "uart4_if"), ("RK_DEBUG", "uart2_if"),
                                   ("CONSOLE", "uart1_if"))) and
            'if(NOT B1_RK_UART STREQUAL "4")' in cmake and
            "B1_LOG_DEFAULT_SOURCE" in cmake and "log_console_process();" in rtos_source and
            "uart_start_all()" in rtos_source,
            "B1须固定UART4协议、USART2日志输入及USART1控制台，不再允许切换协议口")
    require("extern uart_if_t uart4_if;" in interfaces and
            "extern uart_if_t uart2_if;" in interfaces and "extern uart_if_t uart1_if;" in interfaces and
            "uart_tx_mutex_init();" in uart_router and "uart_tx_isr_attach();" in uart_router,
            "B1 UART实例必须归属板级接口，并在启动接收前建立TX锁和中断钩子")
    require('section(".uart_dma")' in uart_router and
            re.search(r"\.uart_dma\s*\(NOLOAD\)\s*:\s*\{[^}]*\}\s*>\s*RAM\b", linker) and
            ".lwip_heap" in linker and "0x30005000" in linker,
            "UART DMA必须放AXI RAM，且显式预留D2中的固定LwIP堆防重叠")
    require(all(f"DMA_REQUEST_{port}_RX" in source for port in ("UART4", "USART2", "USART1")) and
            "DMA1_Stream2_IRQHandler" in interrupts and "USART1_IRQHandler" in interrupts and
            "DMA_NORMAL" in source and "HAL_UARTEx_ReceiveToIdle_DMA" in uart_hw and
            "DMA_IT_HT" in uart_hw and "HAL_UART_RXEVENT_HT" in uart_router and
            "HAL_DMA_IRQHandler" in interrupts and "HAL_UART_IRQHandler" in interrupts,
            "B1 UART RX必须使用NORMAL DMA+IDLE，并忽略HT重复前缀")
    console = (ROOT / "Core/Src/log_console.c").read_text(encoding="utf-8")
    require("uart == &huart4 ? pdMS_TO_TICKS(UART_TX_LOCK_TIMEOUT_MS) : 0U" in uart_hw and
            "uart->Init.Mode = UART_MODE_TX_RX;" in source and "huart2.TxISR = uart_tx_drain;" in uart_hw and
            "USART2.Mode=MODE_TX_RX" in (ROOT / "b1.ioc").read_text(encoding="utf-8") and
            "UART_PORT_RK_PROTOCOL" not in console and '"log mcu"' in console and '"log rk"' in console and
            "HAL_UART_Transmit(" not in source + console and "Core/Src/log_console.c" in cmake,
            "日志须非阻塞且隔离协议，RK调试口须双向且发送不等待，启动日志也必须经过来源选择")
    diagnostics = (ROOT / "Core/Src/board_diagnostics.c").read_text(encoding="utf-8")
    require(source.index("    MX_UART_Init();") < source.index("    MX_I2C_Init();") and
            source.index("    board_diagnostics_boot_finish();") < source.index("    osKernelInitialize();") and
            "s_boot_finished" in diagnostics and "__get_PRIMASK()" in diagnostics and
            "__get_BASEPRI()" in diagnostics and "__get_IPSR()" in diagnostics and
            "20U);" in diagnostics and "board_diagnostics_process();" in rtos_source and
            "Core/Src/board_diagnostics.c" in cmake,
            "早期日志须在UART就绪后、内核创建前有界发送；运行期状态只由默认任务推进")
    require(not re.search(r"\bSCB_(?:Enable|Clean|Invalidate)DCache\w*\s*\(",
                          source + uart_hw + uart_router + formal_tasks),
            "B1保持D-Cache关闭，新增UART/任务不能加入cache维护调用")

    # 精简Boot与单App不依赖升级模块；TCP生命周期与Flash事务保护仍须保留。
    system = (ROOT / "Core/Src/system_stm32h7xx.c").read_text(encoding="utf-8")
    require("ORIGIN = 0x08000000, LENGTH = 128K" in linker and
            "ORIGIN = 0x08020000, LENGTH = 1920K" in linker and
            "ENTRY(Boot_Image_Reset)" in linker and "SCB->VTOR = (uint32_t)g_pfnVectors;" in system and
            "0x000000U" in system and "#define USER_VECT_TAB_ADDRESS" in system and
            "ASSERT(LOADADDR(.tdata) + SIZEOF(.tdata) <= ORIGIN(FLASH) + LENGTH(FLASH)" in linker,
            "合并镜像必须包含128KiB Boot和1920KiB App，VTOR使用实际App向量")
    require(not (ROOT / "App/ota").exists() and
            not (ROOT / "App/protocol/pc/pc_ota.c").exists() and
            "ENABLE_OTA_BOOT" not in cmake and "boot/minimal_boot.cmake" in cmake and
            "OTA_APP_BASE" not in linker and "otaNotReady" not in pc_dispatch,
            "活动工程不应残留MCU升级入口、双槽布局或OTA健康门禁")
    require("pc_dispatch_poll();" in tcp_task and "pc_dispatch_disconnected();" in tcp_task and
            "select(listenfd + 1" in tcp_task and "SO_SNDTIMEO" in tcp_task and
            "flash_transaction_prepare()" in rtos_source and
            "flash_transaction_begin(1000U)" in pc_info,
            "TCP有界等待/断线生命周期与Flash事务保护不得随升级功能删除")
    require("App/tasks/common/task_watchdog.c" in cmake,
            "保留任务存活监督源码，供板级看门狗开关启用")
    print("Boot＋App布局、早期关电、无OTA依赖及只读FRU检查通过")

    manifest_path = args.build_dir / "b1.manifest.json"
    image_path = args.build_dir / "b1.bin"
    payload_path = args.build_dir / "b1.version.bin"
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
    print("独立启动早期关电及PG11防御性控制合同通过")
    print("Safe defaults, SPI2 slave mode 0, SPI4 mode 3 and YT8512H PASS")
    print("Result LED states, 500ms scheduler, active-high PG0-PG5 map and boundary PASS")
    print("W25Q256 32MiB layout, 4-byte QSPI, version/FRU and manifest contract PASS")
    print("FreeRTOS 128KiB heap, LwIP IPv4, YT8512H reset and UID MAC contract PASS")
    print("AD7124-4 driver, SPI4-only binding and conservative mode-3 clock PASS")
    print("MCP4725 Boot map, Fast Write and I2C2-only binding contract PASS")
    print("TPS/PAC/AD monitor, pwr_ctrl and all native DDR steps contract PASS")
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
