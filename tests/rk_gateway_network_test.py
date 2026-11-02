"""临时联调镜像：本机模拟PC TCP客户端和RK串口，执行真实MCU双链路。

只接受带testOnly标志的受理响应；不设置真实供电、不操作B2或真实RK算法。
"""
import argparse
import json
from pathlib import Path
import socket
import struct
import time
import serial
from pc_business_network_test import make_frame, recv_frame


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def rk_frame(sequence, units):
    payload = b''.join(struct.pack('>HH', kind, len(value)) + value for kind, value in units)
    header = b'\x5A' + struct.pack('>H', len(payload))
    body = struct.pack('>Q', sequence) + payload
    return header + struct.pack('<H', crc16(header)) + body + struct.pack('<H', crc16(body)) + b'\xA5'


def rk_result(sequence, command, address=0, die=1, elapsed=987, reverse=False, failed=None):
    # 一个完整结果包；地址和die必须相邻，不能为了换序把二者拆散。
    flag = int(address != 0) if failed is None else failed
    assert flag in (0, 1) and die in (0, 1)
    scalars = [(0x5001, struct.pack('<H', command)), (0x1017, bytes([flag])),
               (0x9002, struct.pack('<Q', elapsed))]
    pairs = [(0x9001, struct.pack('<Q', address)), (0x3001, bytes([die]))] if flag else []
    return rk_frame(sequence, list(reversed(scalars)) + pairs if reverse else scalars[:2] + pairs + scalars[2:])


class Rk:
    def __init__(self, port, events):
        self.port, self.events, self.buffer = port, events, bytearray()
        self.latest_sequence = 0

    def command(self, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.buffer.extend(self.port.read(max(1, self.port.in_waiting)))
            while len(self.buffer) >= 5:
                if self.buffer[0] != 0x5A or crc16(self.buffer[:5]) != 0:
                    del self.buffer[0]
                    continue
                size = int.from_bytes(self.buffer[1:3], 'big')
                if size != 0 and not 4 <= size <= 1024:
                    del self.buffer[0]
                    continue
                if len(self.buffer) < size + 16:
                    break
                wire = bytes(self.buffer[:size + 16]); del self.buffer[:size + 16]
                assert wire[-1] == 0xA5 and crc16(wire[5:-1]) == 0, wire.hex()
                self.events.append({'rkRx': wire.hex()})
                self.latest_sequence = int.from_bytes(wire[5:13], 'big')
                if size == 0 or wire[13:17] == b'\0\0\0\0':
                    continue
                assert size == 6 and wire[13:17] == b'\x50\0\0\x02', wire.hex()
                return self.latest_sequence, int.from_bytes(wire[17:19], 'little')
        return None

    def send(self, wire, split=False):
        self.events.append({'rkTx': wire.hex()})
        if split:
            for byte in wire:
                self.port.write(bytes([byte])); time.sleep(0.002)
        else:
            self.port.write(wire)
        self.port.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='192.168.1.201')
    parser.add_argument('--port', type=int, default=8888)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error('报告已存在，不覆盖历史证据')
    report = {'scope': 'temporary MCU gateway, PC and RK simulated, no physical DDR test',
              'serial': args.serial, 'events': [], 'passed': []}
    def passed(name):
        report['passed'].append(name); print('PASS:', name, flush=True)
    try:
        with serial.Serial(args.serial, args.baud, timeout=0.03, write_timeout=2) as port:
            # 新镜像无在途命令；只丢弃测试开始前的心跳碎片。
            port.reset_input_buffer()
            rk = Rk(port, report['events'])
            with socket.create_connection((args.host, args.port), timeout=3) as sock:
                sock.settimeout(4)
                pc_seq, mcu_seq = 1001, 1
                def send(command, body, fragmented=False, heartbeat=False):
                    nonlocal pc_seq
                    wire = make_frame(command, json.dumps(body, separators=(',', ':')).encode(), pc_seq)
                    pc_seq += 1
                    if heartbeat:
                        wire += make_frame(2, b'{}', pc_seq); pc_seq += 1
                    if fragmented:
                        sock.sendall(wire[:3]); time.sleep(0.02)
                        sock.sendall(wire[3:17]); time.sleep(0.02)
                        sock.sendall(wire[17:])
                    else:
                        sock.sendall(wire)
                def receive(command):
                    nonlocal mcu_seq
                    header, body = recv_frame(sock)
                    report['events'].append({'pcRxCommand': header[4], 'mcuSeq': header[5], 'body': body})
                    assert header[1:4] == (1, 8, 0) and header[4:] == (command, mcu_seq, 0), (header, body)
                    mcu_seq += 1
                    return body
                def start(name, index, **kwargs):
                    send(0x0103, {'algorithmType': 'functional', 'algorithmName': name,
                         'algorithmIndex': index, 'power': {'memoryType': 'LPDDR4X', 'vdd1': '1800',
                         'vdd2': '1100', 'vddq': '600'}, 'reboot': False, 'bootMode': 'emmc',
                         'TimingParameters': {}}, **kwargs)
                    body = receive(6)
                    assert body['status'] == 'accepted' and body['testOnly'] is True
                    assert body['algorithmIndex'] == index
                def check_result(index, status, address):
                    body = receive(0x0105)
                    assert body['algorithmIndex'] == index
                    assert body['siteId'] == 'RK_SIM' and body['testTimeMs'] == 987
                    assert len(body['testResults']) == 2 and body['measurementData'] is None
                    for die, item in enumerate(body['testResults']):
                        assert item['mirrorIndex'] == die and 'siteId' not in item and 'measurementData' not in item
                        assert item['testStatus'] == (status if die == 1 else 1)
                        assert item['resultDesc'] == {'firstErrorAddress': f'0x{address:016X}' if die == 1 and status == 0 else None,
                                                       'errorType': ''}, item

                start('gtest-xor', 101, fragmented=True, heartbeat=True)
                assert receive(1) == {}
                seq, cmd = rk.command(); assert cmd == 3
                assert rk.command(0.8) is None, '不允许500ms自动重发已执行的测试'
                passed('PC半包/粘包到RK小端命令，超过500ms不重发')
                rk.send(rk_result(seq + 100, cmd))
                incomplete = [(0x5001, struct.pack('<H', cmd)), (0x9001, bytes(8)), (0x3001, b'\1')]
                rk.send(rk_frame(seq, incomplete))
                rk.send(rk_frame(seq, incomplete + [incomplete[0], (0x9002, bytes(8))]))
                damaged = bytearray(rk_result(seq, cmd)); damaged[-3] ^= 1
                rk.send(damaged)
                send(2, {}); assert receive(1) == {}
                passed('错Seq/缺字段/重复字段/坏CRC不冒充结果，PC心跳仍正常')
                rk.send(b'\x5A\x99' + rk_result(rk.latest_sequence, cmd), split=True)
                check_result(101, 1, 0)
                passed('伪帧头后恢复、串口逐字节完整结果生成双颗粒0105')
                rk.send(rk_result(rk.latest_sequence, cmd)); send(2, {}); assert receive(1) == {}
                passed('重复最终结果不重复回包，PC与RK Seq互不替代')
                start('mats', 102)
                seq, cmd = rk.command(); assert cmd == 17
                address = 0x100000008
                rk.send(rk_frame(rk.latest_sequence, []) + rk_result(rk.latest_sequence, cmd, address, reverse=True))
                check_result(102, 0, address)
                passed('17=MATS、结果顺序变化、心跳粘包和64位失败地址')
                start('marchx', 103)
                seq, cmd = rk.command(); assert cmd == 18
                send(0x0103, {'algorithmIndex': 104})
                assert receive(6)['errorCode'] == 'invalidArgument'
                sock.settimeout(13)
                body = receive(6)
                assert body['status'] == 'failed' and body['algorithmIndex'] == 103
                assert rk.command(0.3) is None
                rk.send(rk_result(rk.latest_sequence, cmd))
                send(2, {}); assert receive(1) == {}
                passed('显式10秒结果超时，不重发；迟到完成只解除等待、不假报成功')
                start('gtest-rand', 105)
                seq, cmd = rk.command(); assert cmd == 2
                rk.send(rk_result(rk.latest_sequence, cmd)); check_result(105, 1, 0)
                passed('超时后的匹配迟到结果使后续新请求正常执行')
                send(0x0503, {}); assert receive(0x0504) == {'powerState': 'off'}
                passed('全链路期间电源保持OFF，没有真实上电/调压副作用')
        report['success'] = True
    finally:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
