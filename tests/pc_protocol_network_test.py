#!/usr/bin/env python3
"""PC固定头/JSON的TCP验收；板卡须空闲，不执行算法、灯光、电源或升级写入。"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import time


HEADER_SIZE = 18
MAX_PDU = 4096
VERSION = 1
FLAG_JSON = 8


def frame(body: bytes, service: int = 0, command: int = 0x0002,
          sequence: int = 1, version: int = VERSION,
          flag: int = FLAG_JSON, length: int | None = None) -> bytes:
    total = HEADER_SIZE + len(body) if length is None else length
    return struct.pack(">IHHHHIH", total, version, flag, service,
                       command, sequence, 0) + body


def recv_exact(connection: socket.socket, length: int) -> bytes:
    result = bytearray()
    while len(result) < length:
        chunk = connection.recv(length - len(result))
        if not chunk:
            raise AssertionError(f"连接提前关闭，仅收到 {len(result)}/{length} 字节")
        result.extend(chunk)
    return bytes(result)


def expect_response(connection: socket.socket, command: int,
                    sequence: int, expected: dict) -> None:
    fields = struct.unpack(">IHHHHIH", recv_exact(connection, HEADER_SIZE))
    assert HEADER_SIZE <= fields[0] <= MAX_PDU, fields
    assert fields[1:] == (VERSION, FLAG_JSON, 0, command, sequence, 0), fields
    body = recv_exact(connection, fields[0] - HEADER_SIZE)
    actual = json.loads(body.decode("utf-8"))
    assert actual == expected, (actual, expected)


def failure(command: int, error: str) -> dict:
    return {"requestCommandId": command, "status": "failed", "errorCode": error}


def connect(host: str, port: int) -> socket.socket:
    connection = socket.create_connection((host, port), timeout=3.0)
    connection.settimeout(3.0)
    return connection


def expect_disconnect(host: str, port: int, payload: bytes, label: str) -> None:
    with connect(host, port) as connection:
        connection.sendall(payload)
        # 超时只能说明没有收到数据，不能冒充MCU已经主动断连。
        try:
            data = connection.recv(64)
        except (ConnectionResetError, ConnectionAbortedError):
            return
        except socket.timeout as error:
            raise AssertionError(f"{label}: 等待断连超时") from error
        assert data == b"", f"{label}: 预期断连，却收到 {data.hex()}"


def run(host: str, port: int) -> int:
    passed = 0
    # 首值1是当前固件实现的回归预期，不替代双方对重连/回绕规则的最终确认。
    with connect(host, port) as connection:
        pc_seq, mcu_seq = 0x55667788, 1
        connection.sendall(frame(b"{}", command=0x0101, sequence=pc_seq))
        expect_response(connection, 0x0102, mcu_seq, {
            "testerName": "JMCT-4000", "testerVersion": "D00", "testBoard": "RK3588_test",
        })
        print("[PASS] 0101查询W25测试仪信息，0102使用ServiceID=0与MCU独立Seq")
        passed += 1
        pc_seq += 1; mcu_seq += 1

        connection.sendall(frame(b"{}", sequence=pc_seq))
        expect_response(connection, 0x0001, mcu_seq, {})
        print("[PASS] 同连接心跳使用MCU递增Seq，不回显PC流水号")
        passed += 1
        pc_seq += 1; mcu_seq += 1

        # 未登记命令只验帧/JSON，不用真实FUNC请求进行拆包测试。
        body = '{"message":"中文半包"}'.encode("utf-8")
        packet = frame(body, command=0x7FFF, sequence=pc_seq)
        chinese_offset = packet.index("中".encode("utf-8"))
        start = 0
        for end in (3, 11, chinese_offset + 1, len(packet)):
            connection.sendall(packet[start:end])
            start = end
            time.sleep(0.03)
        expect_response(connection, 0x0006, mcu_seq, failure(0x7FFF, "notSupported"))
        print("[PASS] Length/头部/UTF-8字符中间半包，完整后按命令响应")
        passed += 1
        pc_seq += 1; mcu_seq += 1

        connection.sendall(frame(b"{}", sequence=pc_seq) +
                           frame(b"{}", sequence=pc_seq + 1))
        expect_response(connection, 0x0001, mcu_seq, {})
        expect_response(connection, 0x0001, mcu_seq + 1, {})
        print("[PASS] 两帧粘包分别响应，MCU发送Seq连续递增")
        passed += 1
        pc_seq += 2; mcu_seq += 2

        # 合法JSON尾随空白填满4KiB；心跳仍是空对象，不依赖业务回显。
        max_body = b"{}" + b" " * (MAX_PDU - HEADER_SIZE - 2)
        packet = frame(max_body, sequence=pc_seq)
        assert len(packet) == MAX_PDU
        connection.sendall(packet)
        expect_response(connection, 0x0001, mcu_seq, {})
        print("[PASS] 4096字节合法PDU，短心跳响应")
        passed += 1
        pc_seq += 1; mcu_seq += 1

        invalid_prefixes = (
            (b"TEXT", "文本垃圾"),
            (struct.pack(">I", HEADER_SIZE - 1), "Length小于18"),
            (struct.pack(">I", MAX_PDU + 1), "Length超过4096"),
            (frame(b"{}", version=2)[:6], "Version错误"),
            (frame(b"{}", flag=7)[:8], "Flag错误"),
            (frame(b"{}", service=1)[:10], "ServiceID非0"),
        )
        for prefix, label in invalid_prefixes:
            connection.sendall(prefix + frame(b"{}", sequence=pc_seq))
            expect_response(connection, 0x0001, mcu_seq, {})
            print(f"[PASS] {label}丢弃后同连接恢复心跳")
            passed += 1
            pc_seq += 1; mcu_seq += 1

        invalid_bodies = (
            (b"", 0x0002, "空JSON包体"),
            (b'{"x":}', 0x0002, "JSON语法错误"),
            (b'{"x":1,"x":2}', 0x0002, "JSON重复键"),
            (b"[1,2,3]", 0x0002, "JSON根节点不是对象"),
            (b'{"ok":true}x', 0x0002, "JSON尾随数据"),
            (b'{"x":"\xED\xA0\x80"}', 0x0002, "非法UTF-8代理区"),
            (b'{"algorithmName":"mats"}', 0x0103, "FUNC缺少必填参数"),
            (b'{"extra":1}', 0x0002, "心跳JSON不是空对象"),
            (b'{"extra":1}', 0x0101, "测试仪信息查询JSON不是空对象"),
        )
        for body, command, label in invalid_bodies:
            connection.sendall(frame(body, command=command, sequence=pc_seq) +
                               frame(b"{}", sequence=pc_seq + 1))
            expect_response(connection, 0x0006, mcu_seq, failure(command, "invalidArgument"))
            expect_response(connection, 0x0001, mcu_seq + 1, {})
            print(f"[PASS] {label}返回invalidArgument，后续心跳仍正常")
            passed += 1
            pc_seq += 2; mcu_seq += 2

        for command in (0x0001, 0x0102, 0x0105):
            connection.sendall(frame(b"{}", command=command, sequence=pc_seq))
            expect_response(connection, 0x0006, mcu_seq, failure(command, "invalidDirection"))
            passed += 1
            pc_seq += 1; mcu_seq += 1
        print("[PASS] MCU方向命令0001/0102/0105由PC发送时明确拒绝")

    # 只有半帧超时或超出垃圾扫描预算才按合同断连，每次关闭后再建连接。
    disconnect_cases = (
        (frame(b"{}", sequence=100)[:10], "半帧超过500ms字节间空闲"),
        (b"T" * (MAX_PDU + 1), "两次完整帧之间垃圾超过4096字节"),
    )
    for payload, label in disconnect_cases:
        expect_disconnect(host, port, payload, label)
        print(f"[PASS] {label}断连")
        passed += 1

    with connect(host, port) as connection:
        connection.sendall(frame(b"{}", sequence=900))
        expect_response(connection, 0x0001, 1, {})
    print("[PASS] 异常连接关闭后可重连，当前固件MCU Seq重新从1开始")
    passed += 1
    print(f"PC protocol network: {passed} PASS, 0 FAIL")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.1.201")
    parser.add_argument("--port", type=int, default=8888)
    args = parser.parse_args()
    return run(args.host, args.port)


if __name__ == "__main__":
    raise SystemExit(main())
