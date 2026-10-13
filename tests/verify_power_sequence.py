#!/usr/bin/env python3
"""用QEMU运行B1实际电源状态机及联调参数；GPIO/I2C替换为桩，不连接设备。"""
import argparse
from pathlib import Path
import shutil
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--qemu", default=shutil.which("qemu-system-arm") or "D:/qemu/qemu-system-arm.exe")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    boot_tests = root.parent / "bootloader/tests"
    build = root / "build/power-sequence-test"
    build.mkdir(parents=True, exist_ok=True)
    includes = [root / path for path in (
        "Core/Inc", "HAL_Drv", "BSP/TPS62868", "BSP/MCP4725", "App/protocol/pc",
        "Drivers/STM32H7xx_HAL_Driver/Inc",
        "Drivers/CMSIS/Device/ST/STM32H7xx/Include", "Drivers/CMSIS/Include")]
    command = [args.compiler, "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
               "-DSTM32H743xx", "-DUSE_HAL_DRIVER", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-ffunction-sections", "-fdata-sections", *["-I" + str(path) for path in includes],
               str(root / "tests/test_power_sequence.c"), str(root / "HAL_Drv/pwr_ctrl.c"),
               str(boot_tests / "qemu_start.s"), "-nostartfiles", "--specs=nosys.specs",
               "-T" + str(boot_tests / "qemu_test.ld"), "-Wl,--gc-sections", "-o", str(build / "test.elf")]
    subprocess.run(command, check=True, timeout=60)
    result = subprocess.run(
        [args.qemu, "-M", "mps2-an500", "-display", "none", "-monitor", "none", "-serial", "none",
         "-no-reboot", "-semihosting-config", "enable=on,target=native", "-kernel", str(build / "test.elf")],
        capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / "run.log").write_text(output, encoding="utf-8")
    if result.returncode or "PASS: actual B1 power sequence" not in output:
        raise RuntimeError(output)
    print(output.strip())


if __name__ == "__main__":
    main()
