"""仅给本次两份服务器RKAF封装RK3588外层，双层厂商解包后交付；不连接设备。"""
import json
from pathlib import Path

from package_updates import (BUILD, FIT_SHA, LOADERS, PARAMETER_SHA, SOURCE, SOURCE_SHA,
                             TOOLS, TOOL_SHA, execute, file_sha, minimal_manifest,
                             require, sha, source_metadata, verify_update)

OUT = BUILD / 'handoff/update_20261110'
RKAF_SHA = {
    'normal_d4_loader_h1': 'de4900d818cc10bdc8537141bd87cd3e7db5195635344bacf2803012a039e6b0',
    'rk_for_edk2_loader_h1': '41964fd98e737992e7484c7bcbf2bbe54fc46245e27ba5fd3170b6098b134fd9',
}


def main():
    fit_path = BUILD / 'handoff/package_20261110/uboot_h1_reference.img'
    inputs = [SOURCE, fit_path] + [path for path, _ in LOADERS.values()]
    inputs += [TOOLS / name for name in TOOL_SHA]
    inputs += [OUT / variant / 'server-generated/firmware.img' for variant in LOADERS]
    before = {str(path): file_sha(path) for path in inputs}
    require(before[str(SOURCE)] == SOURCE_SHA and before[str(fit_path)] == FIT_SHA, '原始输入散列错误')
    for name, digest in TOOL_SHA.items():
        require(before[str(TOOLS / name)] == digest, '厂商工具身份错误：' + name)
    source_header, _, original_manifest, parameter = source_metadata(SOURCE)
    require(sha(parameter) == PARAMETER_SHA, '原GPT正文散列错误')
    manifest = minimal_manifest(original_manifest)
    fit = fit_path.read_bytes()
    report = {
        'inputs_sha256': before, 'packages': {}, 'hardware_tested': False,
        'method': 'Server afptool 1.62 RKAF; Windows RKImageMaker 2.23 RKFW; Windows vendor unpack cross-check.',
        'source_reports': ['server-build-6ce2981b729a.json', 'server-second-rkaf.json'],
        'limitations': ['Startup payloads only; original full B1 GPT definitions preserved.',
                        'No claim that flashing preserves existing disk data.',
                        'RK auto-entry is user confirmation, not hardware verification.'],
    }
    report_path = OUT / 'verification.json'
    for variant, (loader_path, digest) in LOADERS.items():
        work = OUT / variant / 'server-generated'
        loader = loader_path.read_bytes()
        require(sha(loader) == digest, '输入Loader身份错误')
        for name, data in {'MiniLoaderAll.bin': loader, 'parameter.txt': parameter,
                           'package-file': manifest, 'uboot.img': fit}.items():
            require((work / name).read_bytes() == data, '服务器上传副本与原件不一致：' + name)
        inner = work / 'firmware.img'
        require(before[str(inner)] == RKAF_SHA[variant], '服务器RKAF身份错误：' + variant)
        candidate = work / 'candidate.img'
        commands_path = work / 'tool-commands.json'
        commands = json.loads(commands_path.read_text(encoding='utf-8')) if commands_path.exists() else []
        # normal已在同一次任务中调用工具成功；保留该候选及命令证据，不重复执行。
        if not candidate.exists():
            execute(TOOLS / 'RKImageMaker.exe', ['-RK3588', work / 'MiniLoaderAll.bin', inner, candidate,
                    '-os_type:ANDROIDOS', '-storage:EMMC'], work, commands)
        data = candidate.read_bytes()
        checked = verify_update(data, loader, fit, parameter, manifest, source_header)
        unpacked = work / 'unpacked'
        unpacked.mkdir(exist_ok=True)
        if not (unpacked / 'firmware.img').exists():
            execute(TOOLS / 'RKImageMaker.exe', ['-unpack', candidate, unpacked], work, commands)
        require((unpacked / 'boot.bin').read_bytes() == loader, '厂商解包外层Loader不一致')
        require((unpacked / 'firmware.img').read_bytes() == inner.read_bytes(), '厂商解包RKAF不一致')
        afp = work / 'unpacked-afp'
        afp.mkdir(exist_ok=True)
        if not (afp / 'Image/uboot.img').exists():
            execute(TOOLS / 'AFPTool.exe', ['-unpack', unpacked / 'firmware.img', afp], work, commands)
        for name, expected in {'MiniLoaderAll.bin': loader, 'uboot.img': fit,
                               'parameter.txt': parameter, 'package-file': manifest}.items():
            # Windows AFPTool 2.2将uboot导出到Image子目录；RKAF清单原路径仍原样核对。
            exported = afp / ('Image/uboot.img' if name == 'uboot.img' else name)
            require(exported.read_bytes() == expected, '厂商解包内容不一致：' + name)
        rejected = []
        for label, damaged, expected_loader in (
                ('bad MD5', data[:-1] + bytes([data[-1] ^ 1]), loader),
                ('wrong Loader', data, loader[:-1] + bytes([loader[-1] ^ 1]))):
            try:
                verify_update(damaged, expected_loader, fit, parameter, manifest, source_header)
            except ValueError:
                rejected.append(label)
            else:
                raise ValueError('负向验证未拒绝：' + label)
        final = OUT / ('update_' + variant + '.img')
        final.write_bytes(data)
        require(file_sha(final) == sha(data), '交付文件落盘校验失败')
        checked.update(file=str(final), size=len(data), sha256=sha(data),
                       server_rkaf_sha256=RKAF_SHA[variant], vendor_unpack_matches=True,
                       negative_checks=rejected, tool_commands=commands)
        report['packages'][variant] = checked
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print('PASS', variant, len(data), sha(data), flush=True)
    require(before == {str(path): file_sha(path) for path in inputs}, '原件或工具内容发生变化')
    report.update(local_inputs_unchanged=True, completed=True)
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print('PASS', report_path, flush=True)


if __name__ == '__main__':
    main()
