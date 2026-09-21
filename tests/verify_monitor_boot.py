"""复用GNU/QEMU直接执行正式Boot缓存与MCP4725代码；不证明真实DAC/RK电气行为。"""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--boot-root", type=Path, default=Path(__file__).resolve().parents[2] / "bootloader")
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=Path("D:/qemu/qemu-system-arm.exe"))
    args = parser.parse_args()
    root = args.app_root.resolve()
    build = root / "build/monitor-boot-test"
    build.mkdir(parents=True, exist_ok=True)
    # 只提供未运行的板级/RTOS声明，正式monitor头、状态枚举及DAC驱动均原样编译。
    (build / "boot_test_port.h").write_text('''#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef uint32_t StaticSemaphore_t;
typedef void *SemaphoreHandle_t;
typedef struct { uint32_t unused; } GPIO_TypeDef;
#define GPIOB ((GPIO_TypeDef *)0)
#define GPIO_PIN_12 0x1000U
#define B1_SPI4_NCS_PORT ((GPIO_TypeDef *)0)
#define B1_SPI4_NCS_PIN 0x0800U
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
void vTaskDelay(TickType_t);
uint32_t HAL_GetTick(void);
''', encoding="utf-8")
    for name in ("FreeRTOS", "semphr", "task", "main"):
        (build / f"{name}.h").write_text('#include "boot_test_port.h"\n', encoding="utf-8")
    env = dict(os.environ)
    env["PATH"] = str(args.compiler.parent) + os.pathsep + env.get("PATH", "")
    includes = [build] + [root / part for part in (
        "App/tasks/common", "App/ota", "HAL_Drv", "BSP/MCP4725",
        "BSP/AD7124", "BSP/PAC1934", "BSP/TPS62868")]
    sources = [Path(__file__).with_name("test_monitor_boot.c"),
               root / "App/tasks/common/monitor_task.c", root / "BSP/MCP4725/mcp4725.c",
               args.boot_root / "tests/qemu_start.s"]
    command = [str(args.compiler), "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
               "-g", "-O1", "-DENABLE_OTA_BOOT=1", "-Wall", "-Wextra", "-Werror",
               "-ffunction-sections", "-fdata-sections", *["-I" + str(p) for p in includes],
               *map(str, sources), "-nostartfiles", "--specs=nosys.specs",
               "-T" + str(args.boot_root / "tests/qemu_test.ld"), "-Wl,--gc-sections",
               "-o", str(build / "test.elf")]
    result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=60)
    (build / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run([str(args.qemu), "-M", "mps2-an500", "-display", "none",
                             "-monitor", "none", "-serial", "none", "-no-reboot",
                             "-semihosting-config", "enable=on,target=native",
                             "-kernel", str(build / "test.elf")],
                            capture_output=True, text=True, env=env, timeout=30)
    output = result.stdout + result.stderr
    (build / "run.log").write_text(output, encoding="utf-8")
    if result.returncode or "PASS: actual monitor Boot cache" not in output:
        raise RuntimeError(output)
    print(f"{root.name}: {output.strip()}")


if __name__ == "__main__":
    main()
