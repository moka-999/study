"""离线运行B1实际TIM1回调与HAL_Delay；不连接串口、烧录器或网口。"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-root", type=Path, default=Path("D:/pro/b1"))
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--qemu", default="D:/qemu/qemu-system-arm.exe")
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    root = args.app_root.resolve()
    build = tests.parent / "build/b1-hal-tick-test"
    build.mkdir(parents=True, exist_ok=True)
    hal = root / "Drivers/STM32H7xx_HAL_Driver"
    includes = (root / "Core/Inc", hal / "Inc", root / "Drivers/CMSIS/Include",
                root / "Drivers/CMSIS/Device/ST/STM32H7xx/Include")
    elf = build / "test.elf"
    command = [args.compiler, "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard",
               "-mfpu=fpv5-d16", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-DSTM32H743xx", "-DUSE_HAL_DRIVER", "-ffunction-sections", "-fdata-sections",
               *["-I" + str(path) for path in includes],
               str(tests / "test_b1_hal_tick.c"), str(root / "Core/Src/stm32h7xx_it.c"),
               str(hal / "Src/stm32h7xx_hal.c"), str(hal / "Src/stm32h7xx_hal_tim.c"),
               str(tests / "support/qemu_start.s"), "-nostartfiles", "--specs=nosys.specs",
               "-T" + str(tests / "support/qemu_test.ld"),
               "-Wl,--gc-sections,--defsym=end=_ebss", "-o", str(elf)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (build / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run([args.qemu, "-M", "mps2-an500", "-display", "none", "-monitor", "none",
                             "-serial", "none", "-no-reboot", "-semihosting-config",
                             "enable=on,target=native", "-kernel", str(elf)],
                            capture_output=True, text=True, timeout=15)
    output = result.stdout + result.stderr
    (build / "run.log").write_text(output, encoding="utf-8")
    if result.returncode or "PASS: B1 TIM1 tick" not in output:
        raise RuntimeError(output)
    print(output.strip())


if __name__ == "__main__":
    main()
