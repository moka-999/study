"""用服务器afptool在全新隔离目录生成两份RKAF；外层由本机2.23工具封装，不烧录。"""
import json
import os
from pathlib import Path
import shlex
import struct
import sys
import uuid

# 复用本工程已有SSH依赖目录，凭据仍仅从调用进程的环境变量读取。
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'build/mcu_pxe/deps'))
import paramiko
from package_fit import inspect_slot, split_slots
from package_updates import (BUILD, FIT_SHA, LOADERS, PARAMETER_SHA, SOURCE, SOURCE_SHA,
                             checksum, file_sha, minimal_manifest, require, sha,
                             source_metadata)

HOST = '192.168.1.122'
USER = 'lilihong'
KNOWN_HOSTS = Path('E:/MYZR-RK3588/uefi/tmp/known_hosts')
REMOTE_TOOLS = {
    'afptool': '/disk1/lilihong/tmp_sd_update/tools/afptool',
}
REMOTE_TOOL_SHA = {
    'afptool': '7a10b8ea538e90d6740f141b33a5d8a9883333916fdfc1fc192b4fbfc3ed3216',
}
OUT = BUILD / 'handoff/update_20261110'


def run_remote(client, cwd, argv):
    """调用本次复制的工具并返回日志；调用方先保存退出码再决定是否停止。"""
    command = 'cd ' + shlex.quote(cwd) + ' && ' + ' '.join(shlex.quote(arg) for arg in argv)
    _, stdout, stderr = client.exec_command(command, timeout=120)
    output = stdout.read().decode('utf-8', errors='replace')
    errors = stderr.read().decode('utf-8', errors='replace')
    status = stdout.channel.recv_exit_status()
    return {'command': command, 'exit_code': status, 'stdout': output, 'stderr': errors}


def remote_bytes(sftp, path):
    with sftp.open(path, 'rb') as stream:
        return stream.read()


def upload_bytes(sftp, path, data):
    with sftp.open(path, 'wb') as stream:
        stream.write(data)
    require(sha(remote_bytes(sftp, path)) == sha(data), '上传内容回读不一致：' + path)


def main():
    password = os.environ.get('SERIAL_NET_SSH_PASSWORD')
    require(password, '缺少环境变量 SERIAL_NET_SSH_PASSWORD；不读取或保存其它密码来源')
    require(KNOWN_HOSTS.is_file(), '缺少已核对的known_hosts文件')
    fit_path = BUILD / 'handoff/package_20261110/uboot_h1_reference.img'
    input_paths = [SOURCE, fit_path] + [path for path, _ in LOADERS.values()]
    before = {str(path): file_sha(path) for path in input_paths}
    require(before[str(SOURCE)] == SOURCE_SHA and before[str(fit_path)] == FIT_SHA,
            '原B1整包或H1 FIT散列错误')
    source_header, source_directory, original_manifest, parameter = source_metadata(SOURCE)
    require(sha(parameter) == PARAMETER_SHA, '原GPT正文散列错误')
    manifest = minimal_manifest(original_manifest)
    fit = fit_path.read_bytes()
    inspect_slot(split_slots(fit))
    loaders = {}
    for name, (path, digest) in LOADERS.items():
        data = path.read_bytes()
        require(sha(data) == digest and data[:4] == b'LDR ', 'Loader身份错误：' + name)
        require(checksum(data[:-4]) == struct.unpack_from('<I', data, len(data) - 4)[0],
                'Loader CRC错误：' + name)
        loaders[name] = data

    OUT.mkdir(parents=True, exist_ok=True)
    remote = '/disk1/lilihong/codex-h1-update-20261110-' + uuid.uuid4().hex[:12]
    evidence = {'host': HOST, 'user': USER, 'remote_dir': remote,
                'inputs_sha256': before, 'source_header': source_header,
                'source_directory': source_directory, 'tools': {}, 'commands': [], 'packages': {},
                'hardware_tested': False, 'completed': False,
                'scope': 'New remote task directory only; no source edits, flashing, reset or network services.',
                'limitations': ['Startup payloads only, with all original B1 GPT definitions unchanged.',
                    'This does not prove flashing preserves existing disk data.',
                    'RK auto-entry is user confirmation; Loader/ATF/TEE/H1 execution remains untested.']}
    report_path = OUT / ('server-build-' + remote.rsplit('-', 1)[-1] + '.json')
    def save():
        report_path.write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding='utf-8')
    save()

    client = paramiko.SSHClient()
    client.load_host_keys(str(KNOWN_HOSTS))
    client.set_missing_host_key_policy(paramiko.RejectPolicy())
    try:
        # 仅一次连接，不自动重试、不修改known_hosts、不把密码放进命令或日志。
        client.connect(HOST, username=USER, password=password, look_for_keys=False,
                       allow_agent=False, timeout=12, auth_timeout=12, banner_timeout=12)
        del password
        with client.open_sftp() as sftp:
            sftp.mkdir(remote, mode=0o700)
            tool_dir = remote + '/tools'
            sftp.mkdir(tool_dir, mode=0o700)
            for name, original in REMOTE_TOOLS.items():
                data = remote_bytes(sftp, original)
                require(data.startswith(b'\x7fELF\x01'), '服务器工具不是已核对的32位Linux ELF：' + original)
                require(sha(data) == REMOTE_TOOL_SHA[name], '服务器工具散列与已核对版本不符：' + original)
                destination = tool_dir + '/' + name
                upload_bytes(sftp, destination, data)
                sftp.chmod(destination, 0o700)
                evidence['tools'][name] = {'original_path': original, 'copy_path': destination,
                                           'size': len(data), 'sha256': sha(data)}
                # 无参帮助可能非零，完整保留版本/用法输出；不当作一次打包成功。
                evidence['commands'].append(run_remote(client, remote, [destination]))
                save()
            upload_bytes(sftp, remote + '/source-package-file.txt', original_manifest)
            upload_bytes(sftp, remote + '/source-parameter.txt', parameter)
            for name, loader in loaders.items():
                work = remote + '/' + name
                sftp.mkdir(work, mode=0o700)
                payloads = {'package-file': manifest, 'parameter.txt': parameter,
                            'MiniLoaderAll.bin': loader, 'uboot.img': fit}
                for filename, data in payloads.items():
                    upload_bytes(sftp, work + '/' + filename, data)
                # RESERVED是原清单标记，不上传空文件、不自行添加Linux分区载荷。
                command = [tool_dir + '/afptool', '-pack', '.', 'firmware.img']
                record = run_remote(client, work, command)
                evidence['commands'].append(record)
                upload_bytes(sftp, work + '/afptool.log', (record['stdout'] + record['stderr']).encode('utf-8'))
                save()
                require(record['exit_code'] == 0, '服务器afptool失败；详细输出已保存在本次报告')
                # 仅下载厂商工具生成的RKAF；Linux旧版rkImageMaker不支持本次输入，不再调用。
                local_work = OUT / name / 'server-generated'
                local_work.mkdir(parents=True, exist_ok=True)
                for payload_name, payload_data in payloads.items():
                    (local_work / payload_name).write_bytes(payload_data)
                firmware = remote_bytes(sftp, work + '/firmware.img')
                require(firmware[:4] == b'RKAF', '服务器afptool未生成RKAF')
                local_firmware = local_work / 'firmware.img'
                local_firmware.write_bytes(firmware)
                require(file_sha(local_firmware) == sha(firmware), 'RKAF下载落盘散列错误')
                evidence['packages'][name] = {
                    'rkaf_file': str(local_firmware), 'rkaf_size': len(firmware),
                    'rkaf_sha256': sha(firmware), 'remote_dir': work,
                    'vendor_linux_pack_succeeded': True,
                }
                save()
                print('RKAF', name, len(firmware), sha(firmware), flush=True)
            for name, original in REMOTE_TOOLS.items():
                require(sha(remote_bytes(sftp, original)) == evidence['tools'][name]['sha256'],
                        '服务器原工具内容发生变化：' + original)
            evidence['remote_original_tools_unchanged'] = True
        require(before == {str(path): file_sha(path) for path in input_paths}, '本机输入内容发生变化')
        evidence['local_inputs_unchanged'] = True
        evidence['completed'] = True
        save()
        (OUT / 'rkaf-verification.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding='utf-8')
        print('PASS:', str(report_path), flush=True)
    except Exception as error:
        # 不重试；只记异常类型，避免认证库异常意外带出凭据。
        evidence['failure_type'] = type(error).__name__
        save()
        raise
    finally:
        client.close()


if __name__ == '__main__':
    main()
