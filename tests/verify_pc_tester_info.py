"""在QEMU运行实际测试仪信息与FRU代码；仅模拟Flash介质，不连接设备。"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--without-w25", action="store_true")
    args = parser.parse_args()
    root = args.app_root.resolve()
    support = Path(__file__).with_name("support")
    build = Path(__file__).resolve().parents[1] / "build/pc-tester-info-test" / (root.name + ("-absent" if args.without_w25 else "-present"))
    build.mkdir(parents=True, exist_ok=True)
    compiler = "C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe"
    includes = [root / p for p in ("App/protocol/pc", "BSP/Flash", "HAL_Drv", "Core/Inc", "boot")]
    sources = [Path(__file__).with_name("test_pc_tester_info.c"),
               root / "App/protocol/pc/pc_tester_info.c", root / "BSP/Flash/flash_layout.c",
               root / "boot/boot_fru.c", support / "qemu_start.s"]
    command = [compiler, "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
               "-DBOARD_W25_PRESENT=" + ("0" if args.without_w25 else "1"), "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
               *["-I" + str(p) for p in includes], *map(str, sources), "-nostartfiles",
               "--specs=nosys.specs", "-T" + str(support / "qemu_test.ld"),
               "-Wl,--gc-sections", "-o", str(build / "test.elf")]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (build / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run(["D:/qemu/qemu-system-arm.exe", "-M", "mps2-an500", "-display", "none",
                             "-monitor", "none", "-serial", "none", "-no-reboot",
                             "-semihosting-config", "enable=on,target=native", "-kernel", str(build / "test.elf")],
                            capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / "run.log").write_text(output, encoding="utf-8")
    if result.returncode or ("PASS: absent W25" if args.without_w25 else "PASS: actual tester FRU") not in output:
        raise RuntimeError(output)
    print(root)
    print(output.strip())


if __name__ == "__main__":
    main()
