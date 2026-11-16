"""生成两份仅含启动载荷的RK升级包；保留B1原GPT正文，不连接设备、不烧录。"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from package_fit import inspect_slot, split_slots

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / 'build/mcu_pxe'
SOURCE = Path('C:/Users/Administrator/Desktop/update-marchcx-mats-delta-complete.img')
TOOLS = Path('E:/MYZR-RK3588/发布包-烧录UEFI/02-烧录工具/RKDevTool_Release_v3.15/bin')
SOURCE_SHA = '3d0d85d4f71c659210dbf23e64065e7d43c4d8a761bcb43c54a807cdfd9ef891'
PARAMETER_SHA = '0a87b46be31e98e0fa6c2775bc4996b9fa5dc38fe2ad83b92a2d197eda6972a7'
FIT_SHA = '66f5e6f24da47d9b6c3b596ebd232c2e8167a9fc3cdb772673f0df33eeb23226'
# RKImageMaker 2.23将存储位图写在102字节包头末尾，而非保留区中部。
RKFW_STORAGE_OFFSET = 98
TOOL_SHA = {
    'AFPTool.exe': '685f743429f1767a2c46d12c272bd3976fd0ff647239442a6ba89dfe54915626',
    'RKImageMaker.exe': 'de2f9db5dd5778bca9c344a6f88a390cc72526323f2461d4b77c0e701253bcba'}
LOADERS = {
    'normal_d4_loader_h1': (BUILD / 'recovery_loader_d4_20261104/b1_zhuoyi_20260819_loader.bin',
        '16efb11d4ed14c700749cdc1e95f03a71477aeff89378446f625016c9237d2b4'),
    'rk_for_edk2_loader_h1': (Path('D:/Backup/Downloads/rk3588_spl_for_edk2.bin'),
        '066a2b3f03c78f06f17dd5a22884bf0f0994b5e0e785b62e798a08a192da9091')}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def checksum(data):
    """Rockchip容器CRC：非反射、初值0、无终异或、多项式0x04C10DB7。"""
    table = []
    for value in range(256):
        crc = value << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04C10DB7 if crc & 0x80000000 else 0)) & 0xFFFFFFFF
        table.append(crc)
    crc = 0
    for byte in data:
        crc = ((crc << 8) ^ table[(crc >> 24) ^ byte]) & 0xFFFFFFFF
    return crc


def rkfw_header(header, size):
    require(len(header) >= 102 and header[:4] == b'RKFW', '缺少RKFW头')
    header_size = struct.unpack_from('<H', header, 4)[0]
    version, firmware_version = struct.unpack_from('<II', header, 6)
    chip, loader_offset, loader_size, firmware_offset, firmware_size = struct.unpack_from('<5I', header, 21)
    require(header_size == 102 and loader_offset == 102 and loader_size > 0 and
            firmware_offset == loader_offset + loader_size and firmware_size > 2048 and
            firmware_offset + firmware_size + 32 == size, 'RKFW长度或载荷范围错误')
    return dict(header_size=header_size, version=version, firmware_version=firmware_version,
                chip=chip, loader_offset=loader_offset, loader_size=loader_size,
                firmware_offset=firmware_offset, firmware_size=firmware_size,
                os_type_raw=struct.unpack_from('<I', header, 45)[0],
                storage_type_raw=struct.unpack_from('<I', header, RKFW_STORAGE_OFFSET)[0],
                raw_hex=header[:102].hex())


def rkaf_directory(header, size):
    require(len(header) >= 2048 and header[:4] == b'RKAF', '缺少RKAF头')
    require(struct.unpack_from('<I', header, 4)[0] + 4 == size, 'RKAF声明长度错误')
    count = struct.unpack_from('<I', header, 136)[0]
    require(0 < count <= 16 and 140 + count * 112 <= 2048, 'RKAF目录数量错误')
    entries = {}
    for index in range(count):
        start = 140 + index * 112
        name = header[start:start + 32].split(b'\0')[0].decode('ascii')
        filename = header[start + 32:start + 96].split(b'\0')[0].decode('ascii')
        offset, lba, sectors, length = struct.unpack_from('<4I', header, start + 96)
        require(name and name not in entries and offset + length <= size - 4,
                'RKAF目录重复或组件越界')
        require(length == 0 or offset >= 2048, 'RKAF组件覆盖目录')
        entries[name] = dict(filename=filename, offset=offset, flash_lba=lba,
                             allocation_units_2048=sectors, size=length)
    spans = sorted((item['offset'], item['offset'] + item['size']) for item in entries.values() if item['size'])
    require(all(a[1] <= b[0] for a, b in zip(spans, spans[1:])), 'RKAF组件范围重叠')
    return entries


def parameter_body(data):
    require(data[:4] == b'PARM' and len(data) >= 12, '缺少PARM封装')
    length = struct.unpack_from('<I', data, 4)[0]
    require(length + 12 == len(data), 'PARM长度错误')
    body = data[8:-4]
    require(checksum(body) == struct.unpack_from('<I', data, len(data) - 4)[0], 'PARM CRC错误')
    return body


def source_metadata(path):
    """只提取源目录和参数；原1GB系统分区仅用于来源散列，不装入新包。"""
    with path.open('rb') as stream:
        outer = rkfw_header(stream.read(102), path.stat().st_size)
        stream.seek(outer['firmware_offset'])
        directory = rkaf_directory(stream.read(2048), outer['firmware_size'])
        extracted = {}
        for name in ('package-file', 'parameter'):
            item = directory[name]
            stream.seek(outer['firmware_offset'] + item['offset'])
            extracted[name] = stream.read(item['size'])
    return outer, directory, extracted['package-file'], parameter_body(extracted['parameter'])


def minimal_manifest(original):
    lines = original.splitlines(keepends=True)
    keep = {b'package-file', b'parameter', b'bootloader', b'uboot', b'backup'}
    selected, names = [], set()
    for line in lines:
        fields = line.strip().split()
        if not fields or fields[0].startswith(b'#'):
            selected.append(line)
        elif fields[0] in keep:
            require(len(fields) == 2 and fields[0] not in names, '原清单格式不支持')
            selected.append(line)
            names.add(fields[0])
            if fields[0] == b'backup':
                require(fields[1] == b'RESERVED', 'backup不是原空占位')
    require(names == keep, '原清单缺少预期启动条目或backup占位')
    return b''.join(selected)


def execute(tool, arguments, work, evidence):
    # 老工具用短相对路径，避免内部固定路径缓冲被长绝对目录触发越界。
    command = [str(tool)] + [str(arg.relative_to(work)) if isinstance(arg, Path) and
                             arg.is_relative_to(work) else str(arg) for arg in arguments]
    result = subprocess.run(command, cwd=work, capture_output=True, timeout=120)
    stdout = result.stdout.decode('gb18030', errors='replace')
    stderr = result.stderr.decode('gb18030', errors='replace')
    evidence.append({'command': command, 'cwd': str(work), 'exit_code': result.returncode,
                     'stdout': stdout, 'stderr': stderr})
    (work / 'tool-commands.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding='utf-8')
    require(result.returncode == 0, '厂商工具失败：' + stdout + stderr)


def verify_update(data, loader, fit, parameter, manifest, source_header, expected_storage=2):
    outer = rkfw_header(data[:102], len(data))
    require(data[-32:] == hashlib.md5(data[:-32]).hexdigest().encode('ascii'), 'RKFW MD5错误')
    require(outer['storage_type_raw'] == expected_storage, 'RKFW目标介质不匹配')
    require(outer['chip'] == source_header['chip'] == 0x33353838 and
            outer['version'] == source_header['version'] == 0x01000000,
            'RKImageMaker芯片或容器版本与原B1不一致')
    require(data[outer['loader_offset']:outer['firmware_offset']] == loader, '外层Loader不一致')
    inner = data[outer['firmware_offset']:-32]
    require(checksum(inner[:-4]) == struct.unpack_from('<I', inner, len(inner) - 4)[0], 'RKAF CRC错误')
    directory = rkaf_directory(inner[:2048], len(inner))
    require(set(directory) in ({'package-file', 'parameter', 'bootloader', 'uboot'},
                              {'package-file', 'parameter', 'bootloader', 'uboot', 'backup'}),
            '出现未经允许的分区载荷')
    payloads = {name: inner[item['offset']:item['offset'] + item['size']] for name, item in directory.items()}
    require(payloads['bootloader'] == loader, '内层bootloader不一致')
    require(payloads['uboot'] == fit, '内层H1 FIT不一致')
    require(parameter_body(payloads['parameter']) == parameter, '原GPT正文未原样保留')
    require(payloads['package-file'] == manifest, '最小清单不一致')
    require(directory['uboot']['flash_lba'] == 0x4000 and len(fit) == 0x2000 * 512,
            'uboot地址/容量与原GPT不一致')
    if 'backup' in directory:
        require(payloads['backup'] == b'' and directory['backup']['flash_lba'] == 0x68000,
                'backup应仅是原分区位置的空占位')
    tree, images = inspect_slot(split_slots(fit))
    return {'rkfw': outer, 'directory': directory, 'rkfw_md5_ok': True, 'rkaf_crc_ok': True,
            'parameter_crc_ok': True, 'original_parameter_body_unchanged': True,
            'inner_outer_loader_equal_input': True, 'fit_equal_h1_input': True,
            'fit_dual_slots_identical': True, 'fit_components': images,
            'loader_sha256': sha(loader), 'fit_sha256': sha(fit),
            'parameter_body_sha256': sha(parameter), 'package_file_sha256': sha(manifest)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=BUILD / 'handoff/update_20261110')
    args = parser.parse_args()
    out = args.out.resolve()
    allowed = (BUILD / 'handoff').resolve()
    require(out.is_relative_to(allowed) and out != allowed, '输出必须位于独立handoff构建子目录')
    fit_path = BUILD / 'handoff/package_20261110/uboot_h1_reference.img'
    inputs = [SOURCE, fit_path] + [path for path, _ in LOADERS.values()] + [TOOLS / name for name in TOOL_SHA]
    require(not any(path.resolve().is_relative_to(out) for path in inputs), '输出不可包含输入')
    before = {str(path): file_sha(path) for path in inputs}
    require(before[str(SOURCE)] == SOURCE_SHA and before[str(fit_path)] == FIT_SHA, '原整包或H1 FIT散列错误')
    for name, digest in TOOL_SHA.items():
        require(before[str(TOOLS / name)] == digest, '厂商工具版本散列错误：' + name)
    source_header, source_directory, original_manifest, parameter = source_metadata(SOURCE)
    require(sha(parameter) == PARAMETER_SHA, '原GPT正文散列错误')
    manifest = minimal_manifest(original_manifest)
    fit = fit_path.read_bytes()
    inspect_slot(split_slots(fit))
    out.mkdir(parents=True, exist_ok=True)
    (out / 'source-package-file.txt').write_bytes(original_manifest)
    (out / 'source-parameter.txt').write_bytes(parameter)
    reports = {}
    for variant, (loader_path, digest) in LOADERS.items():
        loader = loader_path.read_bytes()
        require(sha(loader) == digest and loader[:4] == b'LDR ', '输入Loader散列或容器不符')
        require(checksum(loader[:-4]) == struct.unpack_from('<I', loader, len(loader) - 4)[0], 'Loader CRC错误')
        work = out / variant
        pack = work / 'pack'
        pack.mkdir(parents=True, exist_ok=True)
        for name, payload in {'package-file': manifest, 'parameter.txt': parameter,
                              'MiniLoaderAll.bin': loader, 'uboot.img': fit, 'RESERVED': b''}.items():
            (pack / name).write_bytes(payload)
        commands = []
        inner_path = work / 'firmware.img'
        candidate = work / 'candidate.img'
        execute(TOOLS / 'AFPTool.exe', ['-pack', pack, inner_path], work, commands)
        execute(TOOLS / 'RKImageMaker.exe', ['-RK3588', pack / 'MiniLoaderAll.bin', inner_path, candidate,
                '-os_type:ANDROIDOS', '-storage:EMMC'], work, commands)
        data = candidate.read_bytes()
        report = verify_update(data, loader, fit, parameter, manifest, source_header)
        unpacked = work / 'unpacked'
        unpacked.mkdir(exist_ok=True)
        execute(TOOLS / 'RKImageMaker.exe', ['-unpack', candidate, unpacked], work, commands)
        require((unpacked / 'boot.bin').read_bytes() == loader, '厂商解包外层Loader不一致')
        require((unpacked / 'firmware.img').read_bytes() == inner_path.read_bytes(), '厂商解包RKAF不一致')
        afp = work / 'unpacked-afp'
        afp.mkdir(exist_ok=True)
        execute(TOOLS / 'AFPTool.exe', ['-unpack', unpacked / 'firmware.img', afp], work, commands)
        require((afp / 'MiniLoaderAll.bin').read_bytes() == loader and
                (afp / 'uboot.img').read_bytes() == fit and
                (afp / 'parameter.txt').read_bytes() == parameter and
                (afp / 'package-file').read_bytes() == manifest,
                '厂商AFP独立解包与输入不一致')
        # 用已生成包检查坏MD5及错误预期Loader，防止只验证工具返回码。
        negative = []
        for label, damaged, expected_loader in (
            ('corrupted package rejected', data[:-1] + bytes([data[-1] ^ 1]), loader),
            ('wrong loader rejected', data, loader[:-1] + bytes([loader[-1] ^ 1]))):
            try:
                verify_update(damaged, expected_loader, fit, parameter, manifest, source_header)
            except ValueError:
                negative.append(label)
            else:
                raise ValueError('负向检查未拒绝：' + label)
        final = out / ('update_' + variant + '.img')
        candidate.replace(final)
        report.update(file=str(final), size=len(data), sha256=sha(data), loader_crc_ok=True,
                      vendor_unpack_matches=True, negative_checks=negative, tool_commands=commands)
        reports[variant] = report
        (work / 'verification.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    require(before == {str(path): file_sha(path) for path in inputs}, '输入或工具文件发生变化')
    summary = {'source_sha256': SOURCE_SHA, 'source_header': source_header,
               'source_directory': source_directory, 'inputs_and_tools_sha256': before,
               'inputs_unchanged': True, 'tool_versions': {'AFPTool': '2.2', 'RKImageMaker': '2.23'},
               'parameter_body_sha256': PARAMETER_SHA, 'packages': reports,
               'scope': 'RKDevTool upgrade containers with startup payloads only; original full GPT text retained.',
               'hardware_tested': False,
               'limitations': ['No Loader, MCU, RK or storage was written; no power/reset/network service actions.',
                   'Absence of Linux payload does not prove flashing preserves existing disk data.',
                   'New Loader auto-entry is user/RK confirmation, not a hardware result from this task.',
                   'H1 ATF/TEE compatibility and actual boot, DHCP, PXE and MemTest86 remain unverified.']}
    (out / 'verification.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
    for name, report in reports.items():
        print(name, report['size'], report['sha256'])


if __name__ == '__main__':
    main()
