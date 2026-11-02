"""运行正式NOR、DAC、PCF、PHY及MAC链路更新函数的离线故障测试，不访问板卡。"""
import argparse
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--compiler", default=shutil.which("arm-none-eabi-gcc") or "arm-none-eabi-gcc")
    parser.add_argument("--qemu", default=shutil.which("qemu-system-arm") or "D:/qemu/qemu-system-arm.exe")
    args = parser.parse_args()
    root = args.app_root.resolve()
    v2 = Path(__file__).resolve().parents[1]
    build = root / "build/driver-reliability"
    build.mkdir(parents=True, exist_ok=True)
    # 直接提取生产函数参与编译，不另写一份MAC模式切换模型替代被测逻辑。
    source = (root / "LWIP/Target/ethernetif.c").read_text(encoding="utf-8")
    start = source.index("static void ethernet_link_update(void *argument)")
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    (build / "mac_link_under_test.h").write_text(source[start:end], encoding="utf-8")
    assert "tcpip_callback_wait(ethernet_link_update, argument)" in source
    assert "osMutexAcquire(eth_rx_mutex" in source
    includes = [root / p for p in ("Core/Inc", "HAL_Drv", "BSP/MCP4725", "LWIP/Target",
                "Drivers/STM32H7xx_HAL_Driver/Inc", "Drivers/CMSIS/Device/ST/STM32H7xx/Include",
                "Drivers/CMSIS/Include", "Drivers/CMSIS/RTOS2/Include")]
    # PCF为V2独有，两次运行均验证V2文件，不代表B1硬件使用PCF。
    includes += [v2 / "BSP/PCF8574", v2 / "BSP/IIC", build]
    sources = [Path(__file__).with_name("test_driver_reliability.c"),
               root / "HAL_Drv/qspi_hw.c", root / "BSP/MCP4725/mcp4725.c",
               root / "LWIP/Target/eth_custom_phy_interface.c", v2 / "BSP/PCF8574/pcf8574.c",
               Path(__file__).resolve().parent / "support/qemu_start.s"]
    command = [args.compiler, "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
               "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-DSTM32H743xx", "-DUSE_HAL_DRIVER",
               "-ffunction-sections", "-fdata-sections", *["-I" + str(p) for p in includes],
               *map(str, sources), "-nostartfiles", "--specs=nosys.specs",
               "-T" + str(Path(__file__).resolve().parent / "support/qemu_test.ld"), "-Wl,--gc-sections", "-o", str(build / "test.elf")]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (build / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run([args.qemu, "-M", "mps2-an500", "-display", "none", "-monitor", "none",
                             "-serial", "none", "-no-reboot", "-semihosting-config", "enable=on,target=native",
                             "-kernel", str(build / "test.elf")], capture_output=True, text=True, timeout=60)
    output = result.stdout + result.stderr
    (build / "run.log").write_text(output, encoding="utf-8")
    if result.returncode or "PASS: driver reliability" not in output:
        raise RuntimeError(output)
    print(f"{root.name}: {output.strip()}")


if __name__ == "__main__":
    main()
