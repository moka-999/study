#!/usr/bin/env python3
"""编译实际B1槽位读取函数并在QEMU验证；不连接串口、网络或ST-Link。"""
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
    build = root / "build/board-identity-test"
    build.mkdir(parents=True, exist_ok=True)
    includes = [root / path for path in (
        "Core/Inc", "Drivers/STM32H7xx_HAL_Driver/Inc",
        "Drivers/CMSIS/Device/ST/STM32H7xx/Include", "Drivers/CMSIS/Include")]
    command = [args.compiler, "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
               "-DSTM32H743xx", "-DUSE_HAL_DRIVER", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-ffunction-sections", "-fdata-sections", *["-I" + str(path) for path in includes],
               str(root / "tests/test_board_identity.c"), str(boot_tests / "qemu_start.s"),
               "-nostartfiles", "--specs=nosys.specs", "-T" + str(boot_tests / "qemu_test.ld"),
               "-Wl,--gc-sections", "-o", str(build / "test.elf")]
    subprocess.run(command, check=True, timeout=60)
    result = subprocess.run(
        [args.qemu, "-M", "mps2-an500", "-display", "none", "-monitor", "none", "-serial", "none",
         "-no-reboot", "-semihosting-config", "enable=on,target=native", "-kernel", str(build / "test.elf")],
        capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    if result.returncode or "PASS: actual B1 slot reader" not in output:
        raise RuntimeError(output)
    print(output.strip())


if __name__ == "__main__":
    main()
