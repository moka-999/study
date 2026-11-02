"""B1/V2 Boot＋单App的ST-Link下载：核对镜像、备份、写入、回读后复位。"""
from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import zlib

FLASH_BASE = 0x08000000
FLASH_SIZE = 2 * 1024 * 1024
APP_OFFSET = 128 * 1024


def check_image(image: Path, target: str) -> bytes:
    """核验同板型完整Boot＋App；拒绝单独Boot、旧直启App和槽位镜像。"""
    data = image.read_bytes()
    manifest = json.loads(image.with_suffix('.manifest.json').read_text(encoding='utf-8'))
    if manifest.get('target') != target:
        raise RuntimeError('镜像板型不匹配，禁止下载')
    if manifest.get('image_length') != len(data) or manifest.get('image_crc32') != f'0x{zlib.crc32(data):08X}':
        raise RuntimeError('BIN与清单长度/CRC不一致，请重新构建')
    if not APP_OFFSET + 664 <= len(data) <= FLASH_SIZE:
        raise RuntimeError('镜像必须包含128KiB Boot区及完整App向量，总长不超过2MiB')
    stack, entry = struct.unpack_from('<II', data)
    if not 0x20000000 < stack <= 0x20020000 or stack % 8:
        raise RuntimeError('初始栈不在DTCM或未按8字节对齐')
    if not entry & 1 or not FLASH_BASE + 664 <= (entry & ~1) < FLASH_BASE + APP_OFFSET:
        raise RuntimeError('复位入口不在Boot首扇区')
    app_stack, app_entry = struct.unpack_from('<II', data, APP_OFFSET)
    if not 0x20000000 < app_stack <= 0x20020000 or app_stack % 8:
        raise RuntimeError('App初始栈不在DTCM或未按8字节对齐')
    if not app_entry & 1 or not FLASH_BASE + APP_OFFSET + 664 <= (app_entry & ~1) < FLASH_BASE + len(data):
        raise RuntimeError('App入口不在当前合并镜像内')
    # 单看BIN无法识别链接偏移；同时核验ELF的装载地址及实际字节。
    elf = image.with_suffix('.elf').read_bytes()
    if len(elf) < 52 or elf[:7] != b'\x7fELF\x01\x01\x01' or struct.unpack_from('<H', elf, 18)[0] != 40:
        raise RuntimeError('需要同名32位小端ARM ELF')
    elf_entry, phoff = struct.unpack_from('<II', elf, 24)
    phsize, phcount = struct.unpack_from('<HH', elf, 42)
    if phsize != 32 or phoff + phcount * phsize > len(elf) or (elf_entry | 1) != entry:
        raise RuntimeError('ELF程序头或入口不匹配')
    segments = []
    for n in range(phcount):
        kind, offset, _, address, size, _, _, _ = struct.unpack_from('<8I', elf, phoff + n * phsize)
        if kind != 1 or size == 0:
            continue
        if offset + size > len(elf) or not FLASH_BASE <= address < address + size <= FLASH_BASE + len(data):
            raise RuntimeError('ELF下载段超出BIN/片内Flash')
        if elf[offset:offset + size] != data[address - FLASH_BASE:address - FLASH_BASE + size]:
            raise RuntimeError('ELF与BIN内容不同，请重新构建')
        segments.append((address, address + size))
    if not segments or min(s[0] for s in segments) != FLASH_BASE or max(s[1] for s in segments) != FLASH_BASE + len(data):
        raise RuntimeError('合并镜像必须从0x08000000开始且ELF/BIN范围一致')
    return data


def run_cli(programmer, arguments, log):
    """逐步保存输出；超时、非零退出或工具报错立即停止，不自动重试写操作。"""
    try:
        result = subprocess.run([str(programmer), *map(str, arguments)], capture_output=True,
                                text=True, errors='replace', timeout=180)
    except subprocess.TimeoutExpired as error:
        log.write_bytes((error.stdout or b'') + (error.stderr or b''))
        raise RuntimeError(f'工具超时，请检查 {log}') from error
    output = result.stdout + result.stderr
    log.write_text(output, encoding='utf-8')
    print(output)
    if result.returncode or re.search(r'\bError\s*:', output, re.I):
        raise RuntimeError(f'烧录工具失败，见 {log}')
    return output


def select_serial(output, requested):
    """只自动选择唯一探针，多只探针时必须指定序列号。"""
    serials = {v.upper() for v in re.findall(r'(?:ST-?LINK\s+SN|Serial\s+Number)\s*:\s*([0-9A-F]{24})\b', output, re.I)}
    if requested:
        if requested.upper() not in serials:
            raise RuntimeError('指定ST-Link不在枚举结果中')
        return requested.upper()
    if len(serials) != 1:
        raise RuntimeError('未找到唯一ST-Link；请只连接目标板或指定-Serial')
    return serials.pop()


def check_target(output):
    """核对2MiB H743同族、读保护与启动选项；不自动修改选项字节。"""
    if not re.search(r'Device\s+ID\s*:\s*0x0*450\b', output, re.I):
        raise RuntimeError('芯片ID不是0x450，禁止写入')
    sizes = re.findall(r'^[ \t]*(?:Flash|NVM)[ \t]+size[ \t]*:[ \t]*([^\r\n]*)', output, re.I | re.M)
    if not sizes:
        raise RuntimeError('未找到内部Flash容量')
    units = {'bytes': 1, 'kbytes': 1024, 'mbytes': 1024 * 1024}
    for size in sizes:
        found = re.fullmatch(r'([0-9]+)[ \t]*(KBytes|MBytes|Bytes)', size.strip(), re.I)
        if not found or int(found[1]) * units[found[2].lower()] != FLASH_SIZE:
            raise RuntimeError(f'内部Flash不是预期2MiB：{size!r}')
    for name, allowed in (('RDP', {0xAA}), ('SWAP_BANK', {0}), ('BOOT_ADD0', {0x800, FLASH_BASE})):
        found = re.search(r'\b' + name + r'\s*:\s*(0x[0-9a-f]+|[0-9]+)\b', output, re.I)
        if not found or int(found[1], 0) not in allowed:
            raise RuntimeError(f'{name}未知或不符合直接启动条件；不自动修改')


def deploy(programmer, serial, image, target, output_dir):
    """先完整备份内部Flash，再擦写合并镜像；W25由复位后的Boot按配置处理。"""
    data = check_image(image, target)
    output_dir.mkdir(parents=True, exist_ok=False)
    frozen = output_dir / 'app.bin'
    frozen.write_bytes(data)
    print(f'目标板型：{target}；日志和备份：{output_dir}')
    listing = run_cli(programmer, ['-l', 'st-link'], output_dir / '01-probes.log')
    serial = select_serial(listing, serial)
    # B1四针接口没有NRST，使用普通连接/软件复位，不依赖硬件复位线。
    connection = ['-c', 'port=SWD', 'mode=Normal', 'reset=SWrst', 'freq=950', 'sn=' + serial, '-halt']
    target_output = run_cli(programmer, [*connection, '-ob', 'displ'], output_dir / '02-target.log')
    check_target(target_output)
    # Normal连接本身会复位；后续用Hotplug接管已暂停内核，避免校验前启动新镜像。
    connection[2] = 'mode=HOTPLUG'
    backup = output_dir / 'internal-before.bin'
    run_cli(programmer, [*connection, '-u', hex(FLASH_BASE), str(FLASH_SIZE), backup], output_dir / '03-backup.log')
    if not backup.is_file() or backup.stat().st_size != FLASH_SIZE:
        raise RuntimeError('内部Flash备份缺失或不完整，禁止写入')
    (output_dir / 'backup.sha256').write_text(hashlib.sha256(backup.read_bytes()).hexdigest() + '  internal-before.bin\n', encoding='ascii')
    # CubeProgrammer只擦写镜像覆盖的内部扇区；无需全片擦除，也不再运行旧Boot。
    run_cli(programmer, [*connection, '-w', frozen, hex(FLASH_BASE), '-v'], output_dir / '04-program.log')
    readback = output_dir / 'app-readback.bin'
    run_cli(programmer, [*connection, '-u', hex(FLASH_BASE), str(len(data)), readback], output_dir / '05-readback.log')
    if not readback.is_file() or readback.read_bytes() != data:
        raise RuntimeError('镜像回读不一致，未执行最终复位')
    run_cli(programmer, [*connection, '-rst', '-run'], output_dir / '06-start.log')
    print('下载与回读校验完成，已请求复位运行；业务功能仍以实板验证为准。')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--target', choices=['b1-h743', 'apollo-v2-h743'], required=True)
    parser.add_argument('--check-only', action='store_true')
    parser.add_argument('--programmer', type=Path)
    parser.add_argument('--serial', default='')
    parser.add_argument('--confirm-idle', choices=['YES'])
    args = parser.parse_args()
    if not args.check_only and (args.programmer is None or args.confirm_idle != 'YES'):
        parser.error('下载需要--programmer以及--confirm-idle YES；会暂停并复位MCU')
    if args.serial and not re.fullmatch('[0-9a-fA-F]{24}', args.serial):
        parser.error('ST-Link序列号必须为24位十六进制')
    try:
        if args.check_only:
            data = check_image(args.image, args.target)
            print(f'Boot＋App校验通过：Boot=0x08000000，App=0x08020000，{len(data)}字节，{args.target}；未连接硬件。')
        else:
            folder = Path(__file__).resolve().parents[1] / 'build' / datetime.now().strftime('direct-flash-%Y%m%d-%H%M%S-%f')
            deploy(args.programmer, args.serial, args.image, args.target, folder)
    except (OSError, RuntimeError, ValueError, struct.error) as error:
        parser.exit(1, f'FAIL：{error}\n未自动重试或恢复；已连接时MCU可能停止，请保留日志。\n')


if __name__ == '__main__':
    main()
