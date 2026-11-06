"""运行B1实际诊断C代码；板级/串口仅使用测试桩，不连接硬件。"""
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
    build = root / "build/diagnostics-test"
    build.mkdir(parents=True, exist_ok=True)
    headers = ("main", "board_identity", "monitor_task", "uart_router", "b2_spi",
               "b2_task", "b2_service", "rk_link", "FreeRTOS", "task", "lwip/netif",
               "stm32h7xx_hal")
    for header in headers:
        path = build / (header + ".h")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "b1_diagnostics_port.h"\n', encoding="utf-8")
    for default in (0, 1):
        elf = build / f"default-{default}.elf"
        command = [args.compiler, "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard",
                   "-mfpu=fpv5-d16", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-ffunction-sections", "-fdata-sections", f"-DB1_LOG_DEFAULT_SOURCE={default}",
                   *["-I" + str(p) for p in (build, tests / "support", root / "Core/Inc", root / "App/diagnostics")],
                   str(tests / "test_b1_diagnostics.c"), str(root / "Core/Src/board_diagnostics.c"),
                   str(root / "App/diagnostics/protocol_trace.c"),
                   str(tests / "support/qemu_start.s"), "-nostartfiles", "--specs=nosys.specs",
                   "-T" + str(tests / "support/qemu_test.ld"), "-Wl,--gc-sections,--defsym=end=_ebss",
                   "-o", str(elf)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        (build / f"build-{default}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        result = subprocess.run([args.qemu, "-M", "mps2-an500", "-display", "none", "-monitor", "none",
                                 "-serial", "none", "-no-reboot", "-semihosting-config",
                                 "enable=on,target=native", "-kernel", str(elf)],
                                capture_output=True, text=True, timeout=30)
        output = result.stdout + result.stderr
        (build / f"run-{default}.log").write_text(output, encoding="utf-8")
        if result.returncode or "PASS: B1 boot diagnostics" not in output:
            raise RuntimeError(output)
        print(f"default={default}: {output.strip()}")


if __name__ == "__main__":
    main()
