"""生成RKDevTool使用的H1 SPI NOR升级包；只在独立目录打包，不访问板卡。"""
import getpass
import hashlib
import json
from pathlib import Path
import shlex
import sys
import uuid

from package_fit import inspect_slot, split_slots
from package_updates import (BUILD, FIT_SHA, LOADERS, PARAMETER_SHA, SOURCE,
                             TOOLS, TOOL_SHA, checksum, execute, file_sha,
                             parameter_body, require, rkaf_directory,
                             rkfw_header, sha, source_metadata)

sys.path.insert(0, str(BUILD / 'deps'))
import paramiko

HOST = '192.168.1.122'
REMOTE_AFP = '/disk1/lilihong/tmp_sd_update/tools/afptool'
AFP_SHA = '7a10b8ea538e90d6740f141b33a5d8a9883333916fdfc1fc192b4fbfc3ed3216'
KNOWN_HOSTS = Path('E:/MYZR-RK3588/uefi/tmp/known_hosts')
OUT = BUILD / 'handoff/spinor_20261110'
SECTOR_BYTES = 512
NOR_BYTES = 32 * 1024 * 1024
UBOOT_LBA = 0x4000
UBOOT_SECTORS = 0x2000
# 由同一RKImageMaker 2.23分别封装EMMC/SPINOR的包头对照确认。
STORAGE_SPINOR = 0x200
MANIFEST = (b'# NAME\tPATH\npackage-file\tpackage-file\nparameter\tparameter.txt\n'
            b'bootloader\tMiniLoaderAll.bin\nuboot\tuboot.img\n')


def validate(data, payloads, source_header, inner):
    """回读全部载荷、校验和及NOR边界；不把容器有效等同于实板启动成功。"""
    header = rkfw_header(data[:102], len(data))
    require(header['chip'] == source_header['chip'] == 0x33353838, '芯片标识不是RK3588')
    require(header['storage_type_raw'] == STORAGE_SPINOR, '升级包目标不是SPI NOR')
    require(data[-32:] == hashlib.md5(data[:-32]).hexdigest().encode(), 'RKFW MD5错误')
    require(data[header['loader_offset']:header['firmware_offset']] == payloads['MiniLoaderAll.bin'],
            '外层Loader不一致')
    require(data[header['firmware_offset']:-32] == inner, '内包被改写')
    require(checksum(inner[:-4]) == int.from_bytes(inner[-4:], 'little'), 'RKAF CRC错误')
    directory = rkaf_directory(inner[:2048], len(inner))
    require(set(directory) == {'package-file', 'parameter', 'bootloader', 'uboot'},
            '包含计划外分区或载荷')
    for name, item in directory.items():
        actual = inner[item['offset']:item['offset'] + item['size']]
        if name == 'parameter':
            actual = parameter_body(actual)
        require(actual == payloads[item['filename']], '内包载荷错误：' + name)
    require(directory['uboot']['flash_lba'] == UBOOT_LBA, 'NOR的uboot起始扇区错误')
    require(len(payloads['uboot.img']) == UBOOT_SECTORS * SECTOR_BYTES, 'FIT与分区大小不符')
    require((UBOOT_LBA + UBOOT_SECTORS) * SECTOR_BYTES < NOR_BYTES - 34 * SECTOR_BYTES,
            '分区侵入NOR尾部GPT区域')
    _, components = inspect_slot(split_slots(payloads['uboot.img']))
    return {'rkfw': header, 'directory': directory, 'fit_components': components,
            'rkfw_md5_ok': True, 'rkaf_crc_ok': True, 'parameter_crc_ok': True,
            'all_payloads_match': True, 'nor_capacity_bytes': NOR_BYTES,
            'uboot_offset_bytes': UBOOT_LBA * SECTOR_BYTES,
            'uboot_end_exclusive': (UBOOT_LBA + UBOOT_SECTORS) * SECTOR_BYTES}


def main():
    OUT.mkdir(exist_ok=True)
    work = OUT / 'pack'
    work.mkdir(exist_ok=False)
    loader_path, loader_sha = LOADERS['rk_for_edk2_loader_h1']
    fit_path = BUILD / 'handoff/package_20261110/uboot_h1_reference.img'
    require(file_sha(loader_path) == loader_sha and file_sha(fit_path) == FIT_SHA,
            '输入不是已确认的RK Loader及H1后级')
    for name, digest in TOOL_SHA.items():
        require(file_sha(TOOLS / name) == digest, 'Windows工具身份不匹配：' + name)
    source_header, _, _, original_parameter = source_metadata(SOURCE)
    require(sha(original_parameter) == PARAMETER_SHA, 'B1源参数身份不匹配')
    # 保留机器字段，仅给32MiB NOR建立8MiB起始、4MiB大小的uboot分区。
    prefix = original_parameter.split(b'CMDLINE:', 1)[0]
    parameter = prefix + f'CMDLINE: mtdparts=:0x{UBOOT_SECTORS:08x}@0x{UBOOT_LBA:08x}(uboot)\n'.encode()
    payloads = {'MiniLoaderAll.bin': loader_path.read_bytes(), 'uboot.img': fit_path.read_bytes(),
                'package-file': MANIFEST, 'parameter.txt': parameter}
    for name, data in payloads.items():
        (work / name).write_bytes(data)
    report = {'hardware_tested': False, 'source_files_unchanged': False,
              'input_sha256': {name: sha(data) for name, data in payloads.items()},
              'loader_source': str(loader_path), 'fit_source': str(fit_path),
              'layout_basis': 'SPL MTD: lookup uboot partition, fallback CONFIG_MTD_BLK_U_BOOT_OFFS=0x4000.',
              'limitations': ['Current server source is reference, not an exact source match to the supplied Loader.',
                              'SPI NOR cold boot, pin routing and interaction with existing eMMC remain untested.']}
    report_path = OUT / 'verification.json'

    def save():
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    client = paramiko.SSHClient()
    client.load_host_keys(str(KNOWN_HOSTS))
    password = getpass.getpass('SSH password: ')
    client.connect(HOST, username='lilihong', password=password,
                   look_for_keys=False, allow_agent=False, timeout=15)
    del password
    remote = '/disk1/lilihong/codex-h1-spinor-20261110-' + uuid.uuid4().hex[:12]
    report['remote_work_dir'] = remote
    try:
        with client.open_sftp() as sftp:
            with sftp.open(REMOTE_AFP, 'rb') as stream:
                tool = stream.read()
            require(sha(tool) == AFP_SHA, '服务器afptool身份不匹配')
            sftp.mkdir(remote, mode=0o700)
            with sftp.open(remote + '/afptool', 'wb') as stream:
                stream.write(tool)
            sftp.chmod(remote + '/afptool', 0o700)
            for name, data in payloads.items():
                with sftp.open(remote + '/' + name, 'wb') as stream:
                    stream.write(data)
            command = 'cd ' + shlex.quote(remote) + ' && ./afptool -pack . firmware.img'
            _, stdout, stderr = client.exec_command(command, timeout=120)
            record = {'command': command, 'stdout': stdout.read().decode(errors='replace'),
                      'stderr': stderr.read().decode(errors='replace'),
                      'exit_code': stdout.channel.recv_exit_status()}
            report['server_pack'] = record
            save()
            require(record['exit_code'] == 0, '服务器afptool失败，查看verification.json')
            with sftp.open(remote + '/firmware.img', 'rb') as stream:
                inner = stream.read()
            (work / 'firmware.img').write_bytes(inner)
            with sftp.open(REMOTE_AFP, 'rb') as stream:
                require(sha(stream.read()) == AFP_SHA, '服务器原工具内容发生变化')
            report['remote_original_tool_unchanged'] = True
            save()
    finally:
        client.close()

    commands = []
    candidate = work / 'candidate.img'
    execute(TOOLS / 'RKImageMaker.exe', ['-RK3588', work / 'MiniLoaderAll.bin',
            work / 'firmware.img', candidate, '-os_type:ANDROIDOS', '-storage:SPINOR'], work, commands)
    require('storage is spinor' in commands[-1]['stdout'].lower(), '工具未确认SPINOR目标')
    checked = validate(candidate.read_bytes(), payloads, source_header, inner)
    # 对比同一工具的eMMC输出，防止误将已有eMMC包改名交付。
    emmc = BUILD / 'handoff/update_20261110/update_rk_for_edk2_loader_h1.img'
    emmc_header = rkfw_header(emmc.read_bytes()[:102], emmc.stat().st_size)
    require(checked['rkfw']['storage_type_raw'] != emmc_header['storage_type_raw'],
            '存储标识与eMMC包相同')
    unpack = work / 'unpacked'
    unpack.mkdir()
    execute(TOOLS / 'RKImageMaker.exe', ['-unpack', candidate, unpack], work, commands)
    require((unpack / 'boot.bin').read_bytes() == payloads['MiniLoaderAll.bin'], '厂商解包Loader错误')
    require((unpack / 'firmware.img').read_bytes() == inner, '厂商解包内包错误')
    unpack_afp = work / 'unpacked-afp'
    unpack_afp.mkdir()
    execute(TOOLS / 'AFPTool.exe', ['-unpack', unpack / 'firmware.img', unpack_afp], work, commands)
    for name, data in payloads.items():
        path = unpack_afp / ('Image/uboot.img' if name == 'uboot.img' else name)
        require(path.read_bytes() == data, '厂商解包载荷错误：' + name)
    damaged = bytearray(candidate.read_bytes())
    damaged[-1] ^= 1
    try:
        validate(bytes(damaged), payloads, source_header, inner)
    except ValueError:
        checked['damaged_md5_rejected'] = True
    else:
        raise ValueError('检查器未拒绝损坏包')
    final = OUT / 'update_spinor_rk_for_edk2_h1.img'
    final.write_bytes(candidate.read_bytes())
    report.update(checked)
    report.update(output=str(final), size=final.stat().st_size, sha256=file_sha(final),
                  tool_commands=commands, vendor_double_unpack_ok=True)
    require(file_sha(loader_path) == loader_sha and file_sha(fit_path) == FIT_SHA, '原Loader/FIT被修改')
    report['source_files_unchanged'] = True
    report['completed'] = True
    save()
    print('PASS', final, final.stat().st_size, file_sha(final))


if __name__ == '__main__':
    main()
