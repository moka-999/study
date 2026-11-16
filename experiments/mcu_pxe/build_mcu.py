"""生成独立B1实验源码快照并构建，不修改正式工程、不连接烧录器。"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'B1源文件与接入锚点不匹配，拒绝自动修改：{old[:100]}')
    return text.replace(old, new, 1)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--b1', type=Path, default=Path('D:/pro/b1'))
    p.add_argument('--out', type=Path, default=ROOT.parents[1] / 'build/mcu_pxe/b1-experiment')
    p.add_argument('--prepare-only', action='store_true')
    a = p.parse_args()
    src, dest = a.b1.resolve(), a.out.resolve()
    # 输出限定在当前仓库build里，防止误把正式B1作为目标覆盖。
    if not dest.is_relative_to(ROOT.parents[1] / 'build') or dest == src:
        raise ValueError('实验输出必须位于本仓库build目录')
    dest.mkdir(parents=True, exist_ok=True)
    for name in ('Core', 'HAL_Drv', 'BSP', 'App', 'Drivers', 'Middlewares', 'LWIP', 'boot', 'cmake', 'tools'):
        shutil.copytree(src / name, dest / name, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns('build', '__pycache__', '.git'))
    for name in ('CMakeLists.txt', 'STM32H743xx_FLASH.ld', 'startup_stm32h743xx.s'):
        shutil.copy2(src / name, dest / name)
    serial_dir = dest / 'App/serial_net'
    serial_dir.mkdir(parents=True, exist_ok=True)
    for folder in ('common', 'mcu'):
        for f in (ROOT / folder).glob('*.[ch]'):
            shutil.copy2(f, serial_dir / f.name)

    edits = {}
    name = 'CMakeLists.txt'
    text = (dest / name).read_text(encoding='utf-8-sig')
    text += '''
# 此构建目录专用于串口网桥实验，正式B1默认构建完全不含本接入。
target_sources(b1 PRIVATE App/serial_net/serial_net.c App/serial_net/serial_net_mcu.c)
target_include_directories(b1 PRIVATE App/serial_net)
target_compile_definitions(b1 PRIVATE ENABLE_SERIAL_NET_EXPERIMENT=1)
'''
    edits[name] = text
    name = 'Core/Src/freertos.c'
    text = (dest / name).read_text(encoding='utf-8-sig')
    text = replace(text, '#include <stdio.h>', '#include <stdio.h>\n#include "serial_net_mcu.h"')
    text = replace(text, 'if (!b2_service_prepare()) Error_Handler();',
                   'if (!b2_service_prepare() || !serial_net_prepare()) Error_Handler();')
    text = replace(text, 'xTaskCreate(StartRkTask, "rk", 8192, NULL, 3, NULL)',
                   'xTaskCreate(StartSerialNetTask, "serial_net", 2048, NULL, 3, NULL)')
    text = replace(text, 'log_console_start();', '''log_console_start();
    static const char serial_mode[] = "[serial-net] EXPERIMENT; algorithms disabled; power remains OFF\\r\\n";
    (void)log_console_write(serial_mode, sizeof(serial_mode) - 1U);''')
    edits[name] = text
    name = 'LWIP/Target/ethernetif.c'
    text = (dest / name).read_text(encoding='utf-8-sig')
    text = replace(text, '#include "board_identity.h"',
                   '#include "board_identity.h"\n#include "serial_net_mcu.h"')
    # MAC和队列均已就绪，启动ETH收包线程前装上目的地址过滤。
    text = replace(text, 'netif->mtu = ETH_MAX_PAYLOAD;',
                   'netif->mtu = ETH_MAX_PAYLOAD;\n  serial_net_attach(netif);')
    text = replace(text, 'if (netif->input( p, netif) != ERR_OK )',
                   'if (serial_net_input(p) || netif->input(p, netif) != ERR_OK)')
    edits[name] = text
    name = 'App/protocol/pc/pc_test_service.c'
    text = (dest / name).read_text(encoding='utf-8-sig')
    text = replace(text, 'if (!kind || !start || !response) return PC_DISPATCH_ERROR;',
        '''if (!kind || !start || !response) return PC_DISPATCH_ERROR;
    /* UART4由实验网桥独占，算法请求必须在任何电源/队列副作用之前拒绝。 */
    return status_response(request_command, sequence, start->algorithm_index, "notSupported", response);''')
    text = replace(text, 'if (!start || !response) return PC_DISPATCH_ERROR;',
        '''if (!start || !response) return PC_DISPATCH_ERROR;
    /* 首版由050x与EDK菜单独立验收，不把未验证PXE当作正式SLT提交。 */
    return slt_status_response(sequence, "failed", "notSupported", response);''')
    edits[name] = text
    diff = []
    for name, text in edits.items():
        original = (src / name).read_text(encoding='utf-8-sig')
        diff.extend(difflib.unified_diff(original.splitlines(True), text.splitlines(True),
                                       fromfile='a/' + name, tofile='b/' + name))
        (dest / name).write_text(text, encoding='utf-8', newline='\n')
    (dest.parent / 'b1-integration.patch').write_text(''.join(diff), encoding='utf-8')
    provenance = {str(f.relative_to(dest)): hashlib.sha256(f.read_bytes()).hexdigest()
                  for f in dest.rglob('*') if f.is_file() and 'build' not in f.relative_to(dest).parts}
    (dest.parent / 'b1-source-sha256.json').write_text(json.dumps(provenance, indent=2), encoding='utf-8')
    print('独立源码：', dest)
    if a.prepare_only:
        return
    bundle = Path.home() / 'AppData/Local/stm32cube/bundles'
    cmake = bundle / 'cmake/4.3.1+st.1/bin/cmake.exe'
    ninja = bundle / 'ninja/1.13.2+st.1/bin/ninja.exe'
    build = dest / 'build/Debug'
    commands = [[str(cmake), '-S', str(dest), '-B', str(build), '-G', 'Ninja',
                 '-DCMAKE_BUILD_TYPE=Debug', '-DCMAKE_TOOLCHAIN_FILE=' + str(dest / 'cmake/gcc-arm-none-eabi.cmake'),
                 '-DCMAKE_MAKE_PROGRAM=' + str(ninja), '-DPython3_EXECUTABLE=' + sys.executable],
                [str(cmake), '--build', str(build), '-j', '8']]
    with (dest.parent / 'mcu-build.log').open('w', encoding='utf-8') as log:
        for cmd in commands:
            result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=300)
            if result.returncode:
                raise RuntimeError('构建失败，请检查 mcu-build.log')
    artifacts = dest.parent / 'artifacts'
    artifacts.mkdir(exist_ok=True)
    for ext in ('elf', 'bin', 'manifest.json'):
        shutil.copy2(build / ('b1.' + ext), artifacts / ('b1_serial_net.' + ext))
    print('构建通过（未烧录）：', artifacts)


if __name__ == '__main__':
    main()
