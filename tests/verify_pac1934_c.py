"""复用GNU/QEMU执行正式PAC1934、组合层和等待函数；不连接或烧录硬件。"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--qemu", default="D:/qemu/qemu-system-arm.exe")
    args = parser.parse_args()
    root = args.app_root.resolve()
    support = Path(__file__).resolve().parent / "support"
    build = root / "build/pac1934-test"
    build.mkdir(parents=True, exist_ok=True)
    # 仅替换时钟、RTOS和GPIO声明；被测驱动及组合层使用各工程正式源码。
    (build / "test_port.h").write_text("""#pragma once
#include <stdint.h>
typedef struct { uint32_t unused; } GPIO_TypeDef;
typedef uint32_t TickType_t;
uint32_t HAL_GetTick(void);
void vTaskDelay(TickType_t);
""", encoding="utf-8")
    for name in ("main", "FreeRTOS", "task"):
        (build / f"{name}.h").write_text('#include "test_port.h"\n', encoding="utf-8")
    includes = [build] + [root / p for p in ("HAL_Drv", "BSP/AD7124", "BSP/PAC1934", "BSP/TPS62868")]
    sources = [Path(__file__).with_name("test_pac1934.c"), root / "BSP/PAC1934/pac1934.c",
               root / "HAL_Drv/memory_monitor.c", support / "qemu_start.s"]
    command = [args.compiler, "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
               "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-ffunction-sections",
               "-fdata-sections", *["-I" + str(p) for p in includes], *map(str, sources),
               "-nostartfiles", "--specs=nosys.specs", "-T" + str(support / "qemu_test.ld"),
               "-Wl,--gc-sections", "-o", str(build / "test.elf")]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (build / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run([args.qemu, "-M", "mps2-an500", "-display", "none", "-monitor", "none",
                             "-serial", "none", "-no-reboot", "-semihosting-config", "enable=on,target=native",
                             "-kernel", str(build / "test.elf")], capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / "run.log").write_text(output, encoding="utf-8")
    if result.returncode or "PASS: actual PAC1934" not in output:
        raise RuntimeError(output)
    print(f"{root.name}: {output.strip()}")


if __name__ == "__main__":
    main()
