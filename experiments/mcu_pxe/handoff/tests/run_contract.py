"""编译运行真实V2帧与IP门禁；不打开网络、串口或烧录器。"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
repo = root.parents[2]
common = repo.parent / 'b1/App/protocol/serial_net'
build = repo / 'build/same_ip_handoff_20261110/contract-tests'
build.mkdir(parents=True, exist_ok=True)
gcc = Path.home() / 'AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe'
command = [str(gcc), '-mcpu=cortex-m7', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-d16',
           '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
           '-I' + str(common), '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
           str(common / 'serial_net.c'), str(common / 'ip_guard.c'),
           str(root / 'tests/test_contract.c'), str(repo / 'tests/support/qemu_start.s'),
           '-nostartfiles', '--specs=nosys.specs', '-T' + str(repo / 'tests/support/qemu_test.ld'),
           '-o', str(build / 'test.elf')]
subprocess.run(command, check=True, timeout=60)
result = subprocess.run(['D:/qemu/qemu-system-arm.exe', '-M', 'mps2-an500', '-display', 'none',
                         '-monitor', 'none', '-serial', 'none', '-no-reboot',
                         '-semihosting-config', 'enable=on,target=native', '-kernel', str(build / 'test.elf')],
                        capture_output=True, text=True, timeout=30)
output = result.stdout + result.stderr
(build / 'result.log').write_text(output, encoding='utf-8')
print(output.strip())
if result.returncode or 'PASS: handoff contract' not in output:
    raise SystemExit('同IP合同离线验证失败')
