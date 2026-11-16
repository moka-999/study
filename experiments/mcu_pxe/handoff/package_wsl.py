"""在本机WSL生成H1 FIT及所选升级包；外层调用Windows工具，全程无需SSH。"""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path, PureWindowsPath
import shutil
import struct
import subprocess
import sys
from zoneinfo import ZoneInfo

from package_fit import IMAGE_NAMES, negative_checks, replace_bl33
from package_updates import (BUILD, LOADERS, PARAMETER_SHA, RKFW_STORAGE_OFFSET, TOOLS, TOOL_SHA,
                             checksum, file_sha, minimal_manifest, require, sha,
                             verify_update)

sys.path.insert(0, str(Path(__file__).parent / 'edk2'))
from verify_firmware import verify as verify_fv

AFP = BUILD / 'handoff/host-tools/afptool'
AFP_SHA = '7a10b8ea538e90d6740f141b33a5d8a9883333916fdfc1fc192b4fbfc3ed3216'
STORAGE_EMMC = 2
STORAGE_SPINOR = 0x200
NOR_PARAMETER_LINE = b'CMDLINE: mtdparts=:0x00002000@0x00004000(uboot)\n'
RK_LOADER_KEY = 'rk_for_edk2_loader_h1'
RK_LOADER_DIRECTORY = Path('loader/rk')
VARIANTS = {
    'emmc-d4': ('update_emmc_normal_d4_h1.img', 'normal_d4_loader_h1', 'EMMC', STORAGE_EMMC),
    'emmc-rk': ('update_emmc_rk_for_edk2_h1.img', 'rk_for_edk2_loader_h1', 'EMMC', STORAGE_EMMC),
    'spinor-rk': ('update_spinor_rk_for_edk2_h1.img', 'rk_for_edk2_loader_h1', 'SPINOR', STORAGE_SPINOR),
}


def wsl_path(path):
    """把已有Windows工具/输入路径映射到WSL挂载点；Linux路径原样返回。"""
    windows = PureWindowsPath(str(path))
    return Path('/mnt/' + windows.drive[0].lower(), *windows.parts[1:]) if windows.drive else path


def check_tools():
    require(file_sha(AFP) == AFP_SHA, '本机afptool缺失或版本不匹配')
    require(file_sha(wsl_path(TOOLS) / 'RKImageMaker.exe') == TOOL_SHA['RKImageMaker.exe'],
            'Windows RKImageMaker 2.23缺失或版本不匹配')
    require(Path('/proc/sys/fs/binfmt_misc/WSLInterop').exists(), '需要WSL的Windows程序互操作支持')
    require(shutil.which('dumpimage') is not None, '缺少dumpimage')


def read_loaders(edk, variants):
    """RK前级取投放目录内唯一bin；只记录散列，不锁定版本，D4保留原基线。"""
    loaders, sources = {}, {}
    for name in dict.fromkeys(item[1] for item in variants):
        if name == RK_LOADER_KEY:
            directory = edk / RK_LOADER_DIRECTORY
            candidates = sorted(path for path in directory.glob('*')
                                if path.is_file() and path.suffix.lower() == '.bin')
            require(candidates, '请将当前RK完整Loader的.bin文件放入：' + str(directory))
            require(len(candidates) == 1, 'RK前级目录只能保留一份.bin，当前有：' +
                    '、'.join(path.name for path in candidates))
            path, expected = candidates[0], None
        else:
            path, expected = LOADERS[name]
            path = wsl_path(path)
        data = path.read_bytes()
        require(len(data) > 8 and data[:4] == b'LDR ',
                '需要带LDR头的完整Loader，不能直接使用裸SPL：' + str(path))
        require(checksum(data[:-4]) == int.from_bytes(data[-4:], 'little'),
                'Loader CRC错误：' + str(path))
        digest = sha(data)
        if expected is not None:
            require(digest == expected, 'D4基线Loader身份不符：' + str(path))
        loaders[name] = data
        sources[name] = {'path': str(path.resolve()), 'size': len(data), 'sha256': digest}
    return loaders, sources


def run(command, work, records):
    """运行本机打包程序并保存输出；任何一步失败即停止，不访问设备。"""
    result = subprocess.run([str(x) for x in command], cwd=work, capture_output=True, timeout=120)
    record = {'command': [str(x) for x in command], 'cwd': str(work), 'exit_code': result.returncode,
              'stdout': result.stdout.decode('utf-8', errors='replace'),
              'stderr': result.stderr.decode('utf-8', errors='replace')}
    records.append(record)
    (work / 'tool-commands.json').write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding='utf-8')
    require(result.returncode == 0, '工具失败：' + record['stdout'] + record['stderr'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--edk-root', type=Path, default=BUILD.parents[2] / 'edk2')
    parser.add_argument('--variant', choices=('all', *VARIANTS), default='all',
                        help='只打包指定前级和介质；默认all生成三份')
    parser.add_argument('--check-tools', action='store_true')
    args = parser.parse_args()
    check_tools()
    variants = list(VARIANTS.values()) if args.variant == 'all' else [VARIANTS[args.variant]]
    edk = args.edk_root.resolve()
    loader_data, loader_sources = read_loaders(edk, variants)
    for source in loader_sources.values():
        print('LOADER', source['path'], 'SHA256=' + source['sha256'], flush=True)
    if args.check_tools:
        print('PASS 本机工具、WSL互操作和所选Loader检查')
        return
    fv = edk / 'Build/RK3588/DEBUG_GCC5/FV/BL33_AP_UEFI.Fv'
    app = edk / 'Build/RK3588/DEBUG_GCC5/AARCH64/NetReturn.efi'
    fv_report = verify_fv(fv, app)
    base_path = BUILD / 'artifacts/b1_serial_net_D4_network_20261103.img'
    metadata = BUILD / 'handoff/package-source-20261110'
    parameter = (metadata / 'parameter-original.txt').read_bytes()
    require(sha(parameter) == PARAMETER_SHA, '原B1分区参数身份不匹配')
    manifest = minimal_manifest((metadata / 'package-file-original.txt').read_bytes())
    base, new_fv = base_path.read_bytes(), fv.read_bytes()
    fit, _, images, _ = replace_bl33(base, new_fv)
    fit_negatives = negative_checks(base, new_fv)
    stamp = datetime.now(ZoneInfo('Asia/Shanghai')).strftime('%Y%m%d_%H%M%S')
    out = BUILD / ('handoff/wsl_' + stamp)
    out.mkdir(exist_ok=False)
    fit_path = out / 'uboot_h1.img'
    fit_path.write_bytes(fit)
    commands = []
    extract = out / 'fit-extracted'
    extract.mkdir()
    for index, name in enumerate(IMAGE_NAMES):
        target = extract / name
        run(['dumpimage', '-T', 'flat_dt', '-p', str(index), '-o', target, fit_path], out, commands)
        info = images[name]
        require(target.read_bytes() == fit[info['position']:info['position'] + info['size']],
                '独立提取FIT内容不符：' + name)
    report = {'status': 'RUNNING', 'variant': args.variant, 'hardware_tested': False, 'server_used': False,
              'loader_inputs': loader_sources, 'firmware': fv_report,
              'fit_sha256': sha(fit), 'fit_negative_checks': fit_negatives,
              'fit_independent_extraction_ok': True, 'packages': {}}
    source_header = {'chip': 0x33353838, 'version': 0x01000000}
    for filename, loader_name, medium, storage in variants:
        work = out / filename.removesuffix('.img')
        work.mkdir()
        local_parameter, local_manifest = parameter, manifest
        if storage == STORAGE_SPINOR:
            local_parameter = parameter.split(b'CMDLINE:', 1)[0] + NOR_PARAMETER_LINE
            local_manifest = b''.join(line for line in manifest.splitlines(keepends=True)
                                      if not line.strip().startswith(b'backup\t'))
        loader = loader_data[loader_name]
        payloads = {'MiniLoaderAll.bin': loader, 'uboot.img': fit,
                    'parameter.txt': local_parameter, 'package-file': local_manifest}
        for name, data in payloads.items():
            (work / name).write_bytes(data)
        records = []
        run([AFP, '-pack', '.', 'firmware.img'], work, records)
        run([wsl_path(TOOLS) / 'RKImageMaker.exe', '-RK3588', 'MiniLoaderAll.bin',
             'firmware.img', 'candidate.img', '-os_type:ANDROIDOS', '-storage:' + medium], work, records)
        candidate = (work / 'candidate.img').read_bytes()
        checked = verify_update(candidate, loader, fit, local_parameter, local_manifest,
                                source_header, expected_storage=storage)
        checked['parameter_matches_input'] = checked.pop('original_parameter_body_unchanged')
        unpack = work / 'unpacked'
        unpack.mkdir()
        run([wsl_path(TOOLS) / 'RKImageMaker.exe', '-unpack', 'candidate.img', 'unpacked'], work, records)
        require((unpack / 'boot.bin').read_bytes() == loader, '厂商解包Loader不一致')
        require((unpack / 'firmware.img').read_bytes() == (work / 'firmware.img').read_bytes(),
                '厂商解包RKAF不一致')
        afp = work / 'unpacked-afp'
        afp.mkdir()
        run([AFP, '-unpack', 'firmware.img', 'unpacked-afp'], work, records)
        for name, data in payloads.items():
            # afptool将分区镜像解到Image目录，Loader、参数及清单仍放根目录。
            extracted = afp / ('Image/uboot.img' if name == 'uboot.img' else name)
            require(extracted.read_bytes() == data, '厂商解包载荷不一致：' + name)
        # 使用CRC/MD5正确但介质错误的包验证门禁，避免改名包被当作NOR包交付。
        wrong = bytearray(candidate)
        struct.pack_into('<I', wrong, RKFW_STORAGE_OFFSET,
                         STORAGE_EMMC if storage == STORAGE_SPINOR else STORAGE_SPINOR)
        wrong[-32:] = hashlib.md5(wrong[:-32]).hexdigest().encode('ascii')
        try:
            verify_update(bytes(wrong), loader, fit, local_parameter, local_manifest,
                          source_header, expected_storage=storage)
        except ValueError as error:
            require(str(error) == 'RKFW目标介质不匹配', '错误介质检查未在预期条件拒绝')
        else:
            raise ValueError('错误介质包未被拒绝')
        final = out / filename
        final.write_bytes(candidate)
        checked.update(file=str(final), size=len(candidate), sha256=sha(candidate), storage=medium,
                       vendor_double_unpack_ok=True, wrong_storage_rejected=True, commands=records)
        report['packages'][filename] = checked
        (out / 'verification.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print('PASS', filename, len(candidate), sha(candidate), flush=True)
    require(base_path.read_bytes() == base and fv.read_bytes() == new_fv, '输入在打包中发生变化')
    require(all(Path(source['path']).read_bytes() == loader_data[name]
                for name, source in loader_sources.items()), 'Loader在打包中发生变化')
    report['status'] = 'PASS'
    report['inputs_unchanged'] = True
    (out / 'verification.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    (out / 'SHA256SUMS').write_text(''.join(item['sha256'] + '  ' + name + '\n'
                                  for name, item in report['packages'].items()), encoding='ascii')
    (BUILD / 'handoff/wsl-latest.json').write_text(json.dumps(
        {'output': str(out), 'variant': args.variant, 'status': 'PASS'}, indent=2), encoding='utf-8')
    print('OUTPUT', out, flush=True)


if __name__ == '__main__':
    main()
