"""执行实际任务看门狗源码；仅QEMU，不启动烧录器或网络连接。"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--compiler", default="C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe")
    parser.add_argument("--qemu", default="D:/qemu/qemu-system-arm.exe")
    args = parser.parse_args()
    build = args.app_root.resolve() / "build/task-watchdog-test"
    build.mkdir(parents=True, exist_ok=True)
    port = "#pragma once\n#include <stdint.h>\nuint32_t HAL_GetTick(void);\n#define taskENTER_CRITICAL() ((void)0)\n#define taskEXIT_CRITICAL() ((void)0)\n"
    for header in ("main.h", "FreeRTOS.h", "task.h"):
        (build / header).write_text(port, encoding="utf-8")
    support = Path(__file__).resolve().parent / "support"
    common = args.app_root.resolve() / "App/tasks/common"
    elf = build / "test.elf"
    command = [args.compiler, "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
               "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-I"+str(build), "-I"+str(common),
               str(common / "task_watchdog.c"), str(Path(__file__).with_name("test_task_watchdog.c")),
               str(support / "qemu_start.s"), "-nostartfiles", "--specs=nosys.specs",
               "-T"+str(support / "qemu_test.ld"), "-o", str(elf)]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (build / "build.log").write_text(compiled.stdout+compiled.stderr, encoding="utf-8")
    if compiled.returncode:
        raise RuntimeError(compiled.stdout+compiled.stderr)
    result = subprocess.run([args.qemu, "-M", "mps2-an500", "-display", "none", "-monitor", "none",
                             "-serial", "none", "-no-reboot", "-semihosting-config", "enable=on,target=native",
                             "-kernel", str(elf)], capture_output=True, text=True, timeout=30)
    output = result.stdout+result.stderr
    (build / "run.log").write_text(output, encoding="utf-8")
    if result.returncode or "PASS: task watchdog" not in output:
        raise RuntimeError(output)
    print(output.strip())


if __name__ == "__main__":
    main()
