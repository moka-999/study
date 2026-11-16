"""验证代理只服务指定 ARM64 MAC，且不产生地址租约应答；不打开网络端口。"""
from pathlib import Path
import socket
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pxe_proxy as proxy


def request(message=1, client='0.0.0.0', mac=proxy.TARGET_MAC, arch=b'\x00\x0b'):
    packet = bytearray(236)
    packet[:4] = b'\x01\x01\x06\x00'
    packet[4:8] = b'\x74\xb2\x02\xd1'
    packet[12:16] = socket.inet_aton(client)
    packet[28:34] = mac
    return (bytes(packet) + proxy.COOKIE + proxy.option(53, bytes([message]))
            + proxy.option(60, b'PXEClient:Arch:00011:UNDI:003000')
            + proxy.option(93, arch) + b'\xff')


def reply_options(packet):
    """按应答的线上字节独立读取选项，核对没有地址租约配置。"""
    result = {}
    pos = 240
    while packet[pos] != 255:
        tag, size = packet[pos:pos + 2]
        result[tag] = packet[pos + 2:pos + 2 + size]
        pos += 2 + size
    return result


class ProxyTests(unittest.TestCase):
    def test_target_discover_gets_only_boot_info(self):
        reply, destination = proxy.make_reply(request(), 67, ('0.0.0.0', 68))
        self.assertEqual(destination, ('255.255.255.255', 68))
        self.assertEqual(reply[16:20], bytes(4))
        self.assertEqual(reply[20:24], socket.inet_aton(proxy.SERVER))
        self.assertEqual(reply[4:8], request()[4:8])
        fields = reply_options(reply)
        self.assertEqual(fields[53], b'\x02')
        self.assertEqual(fields[67], b'BOOTAA64.efi\x00')
        self.assertEqual(fields[66], b'192.168.1.200')
        self.assertTrue({1, 3, 6, 51, 58, 59}.isdisjoint(fields))
        self.assertIn(proxy.BOOTFILE, reply[108:236])

    def test_address_requests_never_get_ack(self):
        for message in (2, 3, 4, 5, 6, 7, 8):
            self.assertIsNone(proxy.make_reply(request(message), 67, ('0.0.0.0', 68)))

    def test_boot_request_has_zero_yiaddr(self):
        packet = request(3, '192.168.1.31')
        reply, destination = proxy.make_reply(packet, 4011, ('192.168.1.31', 4011))
        self.assertEqual(reply[16:20], bytes(4))
        self.assertEqual(destination, ('192.168.1.31', 4011))
        fields = reply_options(reply)
        self.assertEqual(fields[53], b'\x05')
        self.assertTrue({1, 3, 6, 51, 58, 59}.isdisjoint(fields))

    def test_other_clients_and_formats_are_ignored(self):
        for packet in (request(mac=bytes.fromhex('002233445566')), request(arch=b'\x00\x07'),
                       request()[:-2], request()[:100], request().replace(proxy.COOKIE, bytes(4)),
                       request()[:-1] + proxy.option(53, b'\x01') + b'\xff'):
            self.assertIsNone(proxy.make_reply(packet, 67, ('0.0.0.0', 68)))
        self.assertIsNone(proxy.make_reply(request(), 67, ('0.0.0.0', 1234)))
        self.assertIsNone(proxy.make_reply(request(3), 4011, ('192.168.1.31', 4011)))
        self.assertIsNone(proxy.make_reply(request(3, '192.168.2.31'), 4011, ('192.168.2.31', 4011)))


if __name__ == '__main__':
    unittest.main()
