"""在已知编译服务器的独立目录构建EDK2实验镜像；不烧录、不修改原工程。"""
import argparse
import getpass
import hashlib
import json
import os
from pathlib import Path
import shlex
import sys
import time

ROOT = Path(__file__).resolve().parent
OUT = ROOT.parents[1] / 'build/mcu_pxe'
sys.path.insert(0, str(OUT / 'deps'))
import paramiko


def run(client, command, logfile, timeout=600):
    channel = client.get_transport().open_session()
    channel.exec_command(command)
    started = time.monotonic()
    with logfile.open('wb') as out:
        while True:
            if channel.recv_ready():
                out.write(channel.recv(32768))
            if channel.recv_stderr_ready():
                out.write(channel.recv_stderr(32768))
            if channel.exit_status_ready() and not channel.recv_ready() and not channel.recv_stderr_ready():
                break
            if time.monotonic() - started > timeout:
                channel.close()
                raise TimeoutError(f'远程等待超时，查看 {logfile}；未自动重试')
            time.sleep(0.05)
    if channel.recv_exit_status():
        raise RuntimeError(f'远程构建失败，查看 {logfile}')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host', default='192.168.1.122')
    p.add_argument('--user', default='lilihong')
    p.add_argument('--known-hosts', default='E:/MYZR-RK3588/uefi/tmp/known_hosts')
    p.add_argument('--baseline', default='/disk1/lilihong/uefi-b1-20261026')
    p.add_argument('--remote', default='/disk1/lilihong/uefi-b1-serial-net-20261103')
    a = p.parse_args()
    if a.remote == a.baseline or not a.remote.startswith('/disk1/lilihong/uefi-b1-serial-net-'):
        raise ValueError('输出必须是独立的uefi-b1-serial-net目录')
    OUT.mkdir(parents=True, exist_ok=True)
    client = paramiko.SSHClient()
    client.load_host_keys(a.known_hosts)
    password = os.environ.get('SERIAL_NET_SSH_PASSWORD') or getpass.getpass('编译服务器密码（不保存）：')
    client.connect(a.host, username=a.user, password=password, timeout=10)
    dest, src = shlex.quote(a.remote), shlex.quote(a.baseline)
    try:
        # 用标记阻止误覆盖同名的他人工程；复制文件，不创建会修改原源码的硬链接。
        command = f'''set -eu
if test -e {dest} && ! test -f {dest}/.serial-net-experiment; then exit 20; fi
mkdir -p {dest}
touch {dest}/.serial-net-experiment
for tree in edk2 edk2-platforms edk2-non-osi; do
  mkdir -p {dest}/$tree
  rsync -a --exclude=.git --exclude=Build --exclude=Conf {src}/$tree/ {dest}/$tree/
done
mkdir -p {dest}/Conf
for item in target tools_def build_rule; do
  cp {src}/edk2/Conf/$item.txt {dest}/Conf/$item.txt
done
cp {src}/build-b1.sh {dest}/build-b1.sh
cp {src}/pack-b1.sh {dest}/pack-b1.sh
mkdir -p {dest}/edk2-platforms/Silicon/Rockchip/Drivers/SerialNetSnpDxe
'''
        run(client, command, OUT / 'edk2-stage.log')
        sftp = client.open_sftp()
        driver = a.remote + '/edk2-platforms/Silicon/Rockchip/Drivers/SerialNetSnpDxe/'
        for folder in ('common', 'edk2'):
            for f in (ROOT / folder).iterdir():
                if f.is_file(): sftp.put(str(f), driver + f.name)
        for suffix in ('dsc', 'fdf'):
            path = a.remote + '/edk2-platforms/Platform/Rockchip/RK3588/RK3588.' + suffix
            with sftp.open(path, 'rb') as f: text = f.read().decode('utf-8-sig')
            old = 'Silicon/Rockchip/Drivers/GmacSnpDxe/GmacSnpDxe.inf'
            if text.count(old) != 1: raise ValueError('EDK2组件列表与预期不一致')
            text = text.replace(old, 'Silicon/Rockchip/Drivers/SerialNetSnpDxe/SerialNetSnpDxe.inf')
            if suffix == 'dsc':
                if text.count('B1-PXE-TEST-20261026') != 1:
                    raise ValueError('EDK2版本标记与预期不一致')
                text = text.replace('B1-PXE-TEST-20261026', 'B1-SERIAL-NET-20261103-D4')
            with sftp.open(path, 'wb') as f: f.write(text.encode('utf-8'))
        print('已准备独立EDK2工程：', a.remote, flush=True)
        run(client, f'cd {dest} && bash build-b1.sh && bash pack-b1.sh', OUT / 'edk2-build.log', 900)
        artifacts = OUT / 'artifacts'
        artifacts.mkdir(exist_ok=True)
        files = {'b1_edk2_bl33_20261026.bin': 'b1_serial_net_bl33.bin',
                 'b1_uboot_uefi_test_20261026.img': 'b1_serial_net_reference_fit.img',
                 'reference-fit.its': 'reference-fit.its', 'fit-info.txt': 'fit-info.txt'}
        for remote_name, local_name in files.items():
            sftp.get(a.remote + '/artifacts/' + remote_name, str(artifacts / local_name))
        manifest = {p.name: {'size': p.stat().st_size, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                    for p in artifacts.iterdir() if p.suffix in ('.bin', '.img', '.elf')}
        (artifacts / 'SHA256.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
        print('EDK2构建通过（未烧录）：', artifacts)
    finally:
        client.close()


if __name__ == '__main__':
    main()
