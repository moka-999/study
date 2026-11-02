"""复用项目GNU/QEMU执行真实AD7124/采样组合层；不连接硬件，不改固件或FRU。"""
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
    build = root / "build/ad7124-test"
    build.mkdir(parents=True, exist_ok=True)
    # 只替换GPIO类型及操作；SPI协议、采样和校准均执行正式C代码。
    (build / "main.h").write_text('''#pragma once
#include <stdint.h>
typedef struct { uint32_t unused; } GPIO_TypeDef;
typedef enum { GPIO_PIN_RESET=0, GPIO_PIN_SET=1 } GPIO_PinState;
void HAL_GPIO_WritePin(GPIO_TypeDef *, uint16_t, GPIO_PinState);
void HAL_Delay(uint32_t);
''', encoding="utf-8")
    includes = [build] + [root / p for p in ("HAL_Drv", "BSP/AD7124", "BSP/PAC1934", "BSP/TPS62868")]
    sources = [Path(__file__).with_name("test_ad7124.c"), root / "BSP/AD7124/ad7124.c",
               root / "HAL_Drv/memory_monitor.c", Path(__file__).resolve().parent / "support/qemu_start.s"]
    command = [str(args.compiler), "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
               "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
               "-ffunction-sections", "-fdata-sections", *["-I" + str(p) for p in includes],
               *(["-DAD_TEST_B1=1"] if (root / "b1.ioc").exists() else []),
               *map(str, sources), "-nostartfiles", "--specs=nosys.specs",
               "-T" + str(Path(__file__).resolve().parent / "support/qemu_test.ld"), "-Wl,--gc-sections",
               "-o", str(build / "test.elf")]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (build / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run([args.qemu, "-M", "mps2-an500", "-display", "none", "-monitor", "none",
                             "-serial", "none", "-no-reboot", "-semihosting-config", "enable=on,target=native",
                             "-kernel", str(build / "test.elf")], capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / "run.log").write_text(output, encoding="utf-8")
    if result.returncode or "PASS: actual AD7124" not in output:
        raise RuntimeError(output)
    print(f"{root.name}: {output.strip()}")


if __name__ == "__main__":
    main()
