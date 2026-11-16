"""编译并运行实际C字节流/事务代码；不连接开发板。"""
from pathlib import Path
import argparse
import subprocess

root = Path(__file__).resolve().parents[1]
repo = root.parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--edk2', type=Path, default=Path('E:/MYZR-RK3588/uefi/tmp/gitea-edk2-upload/modified/edk2'))
args = p.parse_args()
headers = args.edk2 / 'MdePkg/Include'
if not (headers / 'Uefi.h').is_file():
    raise FileNotFoundError('需要真实EDK2头文件，请用--edk2指定源码目录')
gcc = Path.home() / 'AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe'
for case in ('wire', 'snp', 'mcu_filter'):
    build = repo / f'build/mcu_pxe/{case}-tests'
    build.mkdir(parents=True, exist_ok=True)
    cmd = [str(gcc), '-mcpu=cortex-m7', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-d16',
           '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'common'),
           '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
           str(root / 'common/serial_net.c'), str(root / f'tests/test_{case}.c'),
           str(repo / 'tests/support/qemu_start.s'), '-nostartfiles', '--specs=nosys.specs',
           '-T' + str(repo / 'tests/support/qemu_test.ld'), '-o', str(build / 'test.elf')]
    if case == 'snp':
        # QEMU工具链默认短枚举；UEFI要求32位枚举。关闭日志后仅放宽日志临时变量告警。
        cmd += ['-DSERIAL_NET_EDK2', '-DMDEPKG_NDEBUG', '-fshort-wchar', '-fno-short-enums',
                '-Wno-unused-but-set-variable',
                '-I' + str(headers), '-I' + str(headers / 'Arm')]
    if case == 'mcu_filter':
        # 真实头文件和pbuf拷贝，ETH仅在测试翻译单元重定向到RAM；不连接设备。
        includes = ['Core/Inc', 'HAL_Drv', 'BSP/TPS62868', 'App/tasks/common',
                    'Drivers/STM32H7xx_HAL_Driver/Inc', 'Drivers/CMSIS/Include',
                    'Drivers/CMSIS/Device/ST/STM32H7xx/Include', 'Drivers/CMSIS/RTOS2/Include',
                    'Middlewares/Third_Party/FreeRTOS/Source/include',
                    'Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F',
                    'Middlewares/Third_Party/LwIP/src/include',
                    'Middlewares/Third_Party/LwIP/system', 'LWIP/Target']
        cmd += ['-DSTM32H743xx', '-DUSE_HAL_DRIVER', '-DUART_PORT_RK_PROTOCOL=0',
                '-I' + str(root / 'tests/stubs')]
        cmd += ['-I' + str(repo / path) for path in includes]
        cmd += [str(repo / 'Middlewares/Third_Party/LwIP/src/core/pbuf.c')]
    subprocess.run(cmd, check=True, timeout=60)
    result = subprocess.run(['D:/qemu/qemu-system-arm.exe', '-M', 'mps2-an500', '-display', 'none',
                             '-monitor', 'none', '-serial', 'none', '-no-reboot',
                             '-semihosting-config', 'enable=on,target=native', '-kernel', str(build / 'test.elf')],
                            capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / 'result.log').write_text(output, encoding='utf-8')
    print(output.strip())
    assert result.returncode == 0 and 'PASS: serial-net' in output
