"""本轮 B1 的临时 PXE 代理：只补启动文件，不分配 IP、不回答其他 MAC。"""
import argparse
import ipaddress
import select
import socket
import time

SERVER = '192.168.1.200'
TARGET_MAC = bytes.fromhex('062ADF952D04')
BOOTFILE = b'BOOTAA64.efi'
COOKIE = bytes.fromhex('63825363')


def parse_request(packet):
    """返回目标 ARM64 PXE 请求的选项；格式错误或其他设备返回 None。"""
    if not 240 <= len(packet) <= 1472:
        return None
    if packet[:4] != b'\x01\x01\x06\x00' or packet[28:34] != TARGET_MAC:
        return None
    if packet[24:28] != bytes(4) or packet[236:240] != COOKIE:
        return None
    result = {}
    pos = 240
    ended = False
    while pos < len(packet):
        tag = packet[pos]
        pos += 1
        if tag == 255:
            ended = True
            break
        if tag == 0:
            continue
        if pos >= len(packet):
            return None
        size = packet[pos]
        pos += 1
        if tag in result or pos + size > len(packet):
            return None
        result[tag] = packet[pos:pos + size]
        pos += size
    if not ended or not result.get(60, b'').startswith(b'PXEClient'):
        return None
    if result.get(93) != b'\x00\x0b':
        return None
    return result


def option(tag, value):
    """生成 DHCP 的单字节 Type/Length 选项，值原样复制。"""
    return bytes((tag, len(value))) + value


def make_reply(packet, port, peer):
    """返回 (应答, 目标地址)；67只回代理OFFER，4011只回启动文件ACK。"""
    fields = parse_request(packet)
    if fields is None:
        return None
    if port == 67 and peer[1] == 68 and fields.get(53) == b'\x01':
        reply_type = b'\x02'
        destination = ('255.255.255.255', 68)
    elif port == 4011 and fields.get(53) == b'\x03':
        client = socket.inet_ntoa(packet[12:16])
        network = ipaddress.ip_network(SERVER + '/24', strict=False)
        if client != peer[0] or ipaddress.ip_address(client) not in network.hosts():
            return None
        if client == SERVER:
            return None
        reply_type = b'\x05'
        destination = peer
    else:
        # 地址请求、续租、释放等全部交给现有公司 DHCP，绝不回 ACK/NAK。
        return None
    response = bytearray(236)
    response[:4] = b'\x02\x01\x06\x00'
    response[4:8] = packet[4:8]
    response[10:16] = packet[10:16]
    # yiaddr 始终为零，声明这是 PXE 代理，没有给客户端分配地址。
    response[20:24] = socket.inet_aton(SERVER)
    response[28:44] = packet[28:44]
    response[108:108 + len(BOOTFILE)] = BOOTFILE
    response.extend(COOKIE)
    response.extend(option(53, reply_type))
    response.extend(option(54, socket.inet_aton(SERVER)))
    response.extend(option(60, b'PXEClient'))
    response.extend(option(66, SERVER.encode('ascii')))
    response.extend(option(67, BOOTFILE + b'\x00'))
    # 回显 UUID 仅用于协议关联；不能用全零 UUID 替代 MAC 白名单。
    if 97 in fields and len(fields[97]) == 17:
        response.extend(option(97, fields[97]))
    response.append(255)
    response.extend(bytes(max(0, 300 - len(response))))
    return bytes(response), destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=1800)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600:
        parser.error('本次临时服务运行时间需在 1～3600 秒内')
    sockets = []
    try:
        for port in (67, 4011):
            connection = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sockets.append(connection)
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            connection.bind(('0.0.0.0' if port == 67 else SERVER, port))
        print(f'READY proxy={SERVER} MAC=06:2A:DF:95:2D:04 file=BOOTAA64.efi; no leases; {args.seconds}s', flush=True)
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            readable, _, _ = select.select(sockets, [], [], 1)
            for connection in readable:
                packet, peer = connection.recvfrom(4096)
                port = connection.getsockname()[1]
                result = make_reply(packet, port, peer)
                if result is None:
                    continue
                response, destination = result
                connection.sendto(response, destination)
                print(f'{time.strftime("%Y-%m-%d %H:%M:%S")} port={port} xid={packet[4:8].hex()} peer={peer} reply={destination} yiaddr=0.0.0.0', flush=True)
    finally:
        for connection in sockets:
            connection.close()
        print('STOPPED proxy; no address leases created', flush=True)


if __name__ == '__main__':
    main()
