"""运行B1/V2正式IDD内部流程C代码；QEMU离线，不连接板卡或服务器。"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--compiler", default="C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe")
    parser.add_argument("--qemu", default="D:/qemu/qemu-system-arm.exe")
    args = parser.parse_args()
    root = args.app_root.resolve()
    build = root / "build/idd-session-test"
    build.mkdir(parents=True, exist_ok=True)
    tests = Path(__file__).resolve().parent
    command = [args.compiler, "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
               "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-ffunction-sections", "-fdata-sections"]
    command += ["-I" + str(root / p) for p in ("App/protocol/rk", "App/protocol/common")]
    command += [str(tests / "test_idd_session.c")]
    command += [str(root / p) for p in ("App/protocol/rk/idd_session.c", "App/protocol/rk/frame.c",
                                       "App/protocol/common/crc.c")]
    command += [str(tests / "support/qemu_start.s"), "-nostartfiles", "--specs=nosys.specs",
                "-T" + str(tests / "support/qemu_test.ld"), "-Wl,--gc-sections", "-o", str(build / "test.elf")]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (build / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run([args.qemu, "-M", "mps2-an500", "-display", "none", "-monitor", "none",
                             "-serial", "none", "-no-reboot", "-semihosting-config", "enable=on,target=native",
                             "-kernel", str(build / "test.elf")], capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / "run.log").write_text(output, encoding="utf-8")
    if result.returncode or "PASS: IDD session" not in output:
        raise RuntimeError(output)
    print(root.name + ": " + output.strip())


if __name__ == "__main__":
    main()
