"""用真实 UEFI 头和驱动源码运行 QEMU 离线合同；不连接设备或服务器。"""

import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--edk2', type=Path, default=Path('D:/pro/edk2/edk2'))
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    repo = source.parents[3]
    headers = args.edk2 / 'MdePkg/Include'
    if not (headers / 'Uefi.h').is_file():
        raise FileNotFoundError('需要真实 EDK2 头文件，请用 --edk2 指定源码目录')
    build = repo / 'build/mcu_pxe/handoff-snp-tests'
    build.mkdir(parents=True, exist_ok=True)
    compiler = (Path.home() / 'AppData/Local/stm32cube/bundles/'
                'gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe')
    common = repo.parent / 'b1/App/protocol/serial_net'
    # ARM QEMU 用于纯 C 合同；实际 AArch64 模块仍须单独通过 EDK2 构建。
    command = [str(compiler), '-mcpu=cortex-m7', '-mthumb', '-mfloat-abi=hard',
               '-mfpu=fpv5-d16', '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
               '-Werror', '-ffunction-sections', '-fdata-sections',
               '-Wl,--gc-sections', '-DSERIAL_NET_EDK2', '-DMDEPKG_NDEBUG',
               '-fshort-wchar', '-fno-short-enums', '-Wno-unused-but-set-variable',
               '-I' + str(common), '-I' + str(headers), '-I' + str(headers / 'Arm'),
               '-I' + str(args.edk2 / 'MdeModulePkg/Include'),
               '-I' + str(args.edk2.parent / 'edk2-platforms/Silicon/Rockchip/Include'),
               '-I' + str(args.edk2.parent / 'edk2-platforms/Silicon/Rockchip/RK3588/Include'),
               str(common / 'serial_net.c'), str(common / 'ip_guard.c'),
               str(source / 'test_snp_handoff.c'), str(repo / 'tests/support/qemu_start.s'),
               '-nostartfiles', '--specs=nosys.specs',
               '-T' + str(repo / 'tests/support/qemu_test.ld'),
               '-o', str(build / 'test.elf')]
    for case, elf, log, passed in (
        ('test_snp_handoff.c', 'test.elf', 'result.log', 'PASS: same-IP SNP handoff'),
        ('test_slt_auto.c', 'slt_auto.elf', 'slt_auto.log', 'PASS: authorized SLT auto boot'),
        ('test_dhcp_handoff.c', 'dhcp.elf', 'dhcp.log', 'PASS: DHCP authorized Option 50'),
    ):
        current = [str(source / case) if item == str(source / 'test_snp_handoff.c') else
                   str(build / elf) if item == str(build / 'test.elf') else item for item in command]
        subprocess.run(current, check=True, timeout=60)
        result = subprocess.run(
            ['D:/qemu/qemu-system-arm.exe', '-M', 'mps2-an500', '-display', 'none',
             '-monitor', 'none', '-serial', 'none', '-no-reboot',
             '-semihosting-config', 'enable=on,target=native', '-kernel', str(build / elf)],
            capture_output=True, text=True, timeout=60)
        output = result.stdout + result.stderr
        (build / log).write_text(output, encoding='utf-8')
        print(output.strip())
        if result.returncode != 0 or passed not in output:
            raise RuntimeError('EDK离线合同失败，查看 ' + str(build / log))


if __name__ == '__main__':
    main()
