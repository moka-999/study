"""编译并执行B1真实分流模块；仅QEMU离线验证，不访问板卡或网络。"""
from pathlib import Path
import subprocess


def main():
    source = Path(__file__).resolve().parent
    repo = source.parents[3]
    common = repo.parent / 'b1/App/protocol/serial_net'
    build = repo / 'build/mcu_pxe/handoff/net-mux-tests'
    build.mkdir(parents=True, exist_ok=True)
    compiler = (Path.home() / 'AppData/Local/stm32cube/bundles/'
                'gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe')
    elf = build / 'test.elf'
    command = [str(compiler), '-mcpu=cortex-m7', '-mthumb', '-mfloat-abi=hard',
               '-mfpu=fpv5-d16', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-I' + str(common), str(common / 'net_mux.c'), str(common / 'ip_guard.c'),
               str(source / 'test_net_mux.c'), str(repo / 'tests/support/qemu_start.s'),
               '-nostartfiles', '--specs=nosys.specs', '-T' + str(repo / 'tests/support/qemu_test.ld'),
               '-o', str(elf)]
    subprocess.run(command, check=True, timeout=60)
    result = subprocess.run(['D:/qemu/qemu-system-arm.exe', '-M', 'mps2-an500',
                             '-display', 'none', '-monitor', 'none', '-serial', 'none',
                             '-no-reboot', '-semihosting-config', 'enable=on,target=native',
                             '-kernel', str(elf)], capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / 'result.log').write_text(output, encoding='utf-8')
    print(output, end='')
    if result.returncode or 'PASS: actual B1 same-IP net mux' not in output:
        raise RuntimeError('分流合同失败：' + str(build / 'result.log'))


if __name__ == '__main__':
    main()
