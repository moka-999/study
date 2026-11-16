"""按连接对端 IP 更新 Tftpd64 静态绑定；仅改指定 INI，不连接设备或启动服务。"""

import argparse
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import ipaddress
import json
import os
from pathlib import Path
import re


DHCP_SECTION = 'DHCP'
SERVER_SECTION = 'TFTPD32'
LOCK_SECTION = 'SLT Binding Locks'
DHCP_SERVICE_BIT = 4
# Tftpd64 4.74 的 inet_addr 返回 INADDR_NONE，且非空键不会回退旧注册表值。
DISABLED_BINDING = '255.255.255.255'
INI_BUFFER_CHARS = 32768
MAC_PATTERN = re.compile(r'(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}')
SESSION_PATTERN = re.compile(r'[A-Za-z0-9_.-]{1,64}')


def normalize_mac(value):
    """接收六字节冒号分隔单播 MAC，返回 Tftpd64 使用的大写键；不从 IP 推导。"""
    if not isinstance(value, str) or not MAC_PATTERN.fullmatch(value):
        raise ValueError('RK MAC 必须为六字节冒号格式，例如 06:2A:DF:95:2D:04')
    raw = bytes.fromhex(value.replace(':', ''))
    if not any(raw) or raw[0] & 1:
        raise ValueError('RK MAC 必须为非零单播地址')
    return value.upper()


def _api():
    """使用与 Tftpd64 相同的 Windows INI 接口，避免整文件重写或依赖缓存刷新猜测。"""
    if os.name != 'nt':
        raise OSError('本辅助工具需要 Windows，与当前 Tftpd64 的 INI 接口配套')
    api = ctypes.WinDLL('kernel32', use_last_error=True)
    api.GetPrivateProfileSectionW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR,
                                             wintypes.DWORD, wintypes.LPCWSTR]
    api.GetPrivateProfileSectionW.restype = wintypes.DWORD
    api.WritePrivateProfileStringW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR,
                                              wintypes.LPCWSTR, wintypes.LPCWSTR]
    api.WritePrivateProfileStringW.restype = wintypes.BOOL
    return api


def _section(path, section):
    """读取一个 INI 节；拒绝重复键和截断，不把不完整配置当作有效地址表。"""
    buffer = ctypes.create_unicode_buffer(INI_BUFFER_CHARS)
    count = _api().GetPrivateProfileSectionW(section, buffer, len(buffer), str(path))
    if count >= len(buffer) - 2:
        raise ValueError(f'INI 节过大：{section}')
    result = {}
    for entry in buffer[:count].split('\0'):
        if not entry:
            continue
        key, sep, value = entry.partition('=')
        key = key.strip().upper()
        if not sep or not key or key in result:
            raise ValueError(f'INI 节存在非法或重复键：{section}')
        result[key] = value.strip()
    return result


def _session(value):
    if not isinstance(value, str) or not SESSION_PATTERN.fullmatch(value):
        raise ValueError('session 必须为 1..64 个 ASCII 字母、数字、点、横线或下划线')
    return value


def _host(value, network, server_ip):
    address = ipaddress.IPv4Address(value)
    if (address not in network or address in (network.network_address,
                                             network.broadcast_address)
            or address == server_ip or address.is_multicast or address.is_loopback
            or address.is_unspecified or address.is_reserved):
        raise ValueError('MCU IP 必须为该网段有效主机地址，且不能等于服务器地址')
    return str(address)


def read_bindings(path):
    """返回磁盘配置及登记锁；DHCP 位仅表示配置值，不证明服务正在运行。"""
    path = Path(path).resolve(strict=True)
    dhcp = _section(path, DHCP_SECTION)
    server = _section(path, SERVER_SECTION)
    if dhcp.get('POOLSIZE') != '0':
        raise ValueError('本方案要求 PoolSize=0，仅允许已登记的静态 MAC')
    server_text = server.get('DHCP LOCALIP') or server.get('LOCALIP')
    if not server_text or 'MASK' not in dhcp:
        raise ValueError('INI 缺少明确的服务器 IPv4 地址或 DHCP Mask')
    server_ip = ipaddress.IPv4Address(server_text)
    network = ipaddress.IPv4Network(f'{server_ip}/{dhcp["MASK"]}', strict=False)
    if (network.prefixlen not in range(1, 31) or server_ip.is_multicast
            or server_ip.is_loopback or server_ip.is_unspecified or server_ip.is_reserved
            or server_ip in (network.network_address, network.broadcast_address)):
        raise ValueError('服务器地址和掩码不能用于当前 IPv4 DHCP 网络')
    bindings = {}
    for key, value in dhcp.items():
        if MAC_PATTERN.fullmatch(key):
            mac = normalize_mac(key)
            if not value or value == DISABLED_BINDING:
                continue
            address = _host(value, network, server_ip)
            if address in bindings.values():
                raise ValueError(f'多个 RK 绑定了同一 IP：{address}')
            bindings[mac] = address
        elif ':' in key:
            raise ValueError(f'非法 MAC 配置键：{key}')
    locks = _section(path, LOCK_SECTION)
    for key, value in locks.items():
        normalize_mac(key)
        _session(value)
        if key not in bindings:
            raise ValueError('存在无对应地址绑定的 SLT 锁')
    services = int(server.get('SERVICES', '0'), 10)
    return {'server_ip': str(server_ip), 'network': str(network),
            'dhcp_enabled': bool(services & DHCP_SERVICE_BIT),
            'bindings': bindings, 'locks': locks}


@contextmanager
def _exclusive(path):
    """同一 INI 的工具写操作串行；异常退出遗留锁不自动清除，防止误认为可交接。"""
    lock_path = path.with_name(path.name + '.lock')
    with lock_path.open('x', encoding='ascii') as handle:
        try:
            handle.write(str(os.getpid()))
            handle.flush()
            yield
        finally:
            handle.close()
            lock_path.unlink()


def _write(path, section, key, value):
    """逐键写入并以同类系统接口回读；value=None 删除键，不改其它服务配置。"""
    if not _api().WritePrivateProfileStringW(section, key, value, str(path)):
        raise ctypes.WinError(ctypes.get_last_error())
    actual = _section(path, section).get(key)
    if actual != value:
        raise OSError('INI 写入后回读不一致；不得据此允许 RK 接管地址')


def _change(path, mac, action, value=None, expected_ip=None):
    path = Path(path).resolve(strict=True)
    mac = normalize_mac(mac)
    with _exclusive(path):
        state = read_bindings(path)
        current = state['bindings'].get(mac)
        owner = state['locks'].get(mac)
        if action == 'bind':
            value = _host(value, ipaddress.IPv4Network(state['network']),
                          ipaddress.IPv4Address(state['server_ip']))
            if owner and value != current:
                raise ValueError('本轮 SLT 已锁定地址，不能在测试期间改绑')
            if any(ip == value and other != mac for other, ip in state['bindings'].items()):
                raise ValueError('该 IP 已登记给另一块 RK')
            if value != current:
                _write(path, DHCP_SECTION, mac, value)
        elif action == 'remove':
            if owner:
                raise ValueError('本轮 SLT 已锁定地址，不能删除绑定')
            _write(path, DHCP_SECTION, mac, DISABLED_BINDING)
        else:
            value = _session(value)
            if current is None:
                raise ValueError('必须先登记该 RK 的 MCU IP')
            if action == 'lock':
                expected_ip = _host(expected_ip, ipaddress.IPv4Network(state['network']),
                                    ipaddress.IPv4Address(state['server_ip']))
                if current != expected_ip:
                    raise ValueError('登记 IP 与本次连接不一致，不能锁定过期连接的配置')
                if owner and owner != value:
                    raise ValueError('已有其它 SLT session 持有该绑定')
                if owner is None:
                    _write(path, LOCK_SECTION, mac, value)
            elif action == 'unlock':
                if owner != value:
                    raise ValueError('session 与当前 SLT 锁不匹配')
                _write(path, LOCK_SECTION, mac, None)
            else:
                raise ValueError('未知配置操作')
        return read_bindings(path)


def update_binding(path, mac, ip):
    """供连接成功回调调用：ip 为现有 TCP 对端 IPv4，mac 为已确认的 RK MAC。"""
    return _change(path, mac, 'bind', ip)


def lock_binding(path, mac, session, expected_ip):
    """核对本次连接 IP 后冻结配置；只记录锁，不代表 MCU 已释放 IP 或 EDK 获许可。"""
    return _change(path, mac, 'lock', session, expected_ip)


def unlock_binding(path, mac, session):
    """由确认 RK 已退出的调用方解锁；不使用断线、DHCP RELEASE 或超时替代确认。"""
    return _change(path, mac, 'unlock', session)


def remove_binding(path, mac):
    """禁用空闲旧配对；写 INADDR_NONE 阻止 Tftpd64 回退旧注册表项，不解除在途锁。"""
    return _change(path, mac, 'remove')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ini', required=True, type=Path, help='明确指定 INI，验证时使用副本')
    commands = parser.add_subparsers(dest='action', required=True)
    commands.add_parser('show', help='显示登记表；不探测服务或设备')
    for action in ('bind', 'lock', 'unlock', 'remove'):
        command = commands.add_parser(action)
        command.add_argument('--rk-mac', required=True)
        if action in ('bind', 'lock'):
            command.add_argument('--mcu-ip', required=True)
        if action in ('lock', 'unlock'):
            command.add_argument('--session', required=True)
    args = parser.parse_args()
    try:
        if args.action == 'show':
            result = read_bindings(args.ini)
        elif args.action == 'bind':
            result = update_binding(args.ini, args.rk_mac, args.mcu_ip)
        elif args.action == 'remove':
            result = remove_binding(args.ini, args.rk_mac)
        elif args.action == 'lock':
            result = lock_binding(args.ini, args.rk_mac, args.session, args.mcu_ip)
        else:
            result = unlock_binding(args.ini, args.rk_mac, args.session)
    except (OSError, ValueError) as error:
        parser.exit(2, f'失败：{error}\n')
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
