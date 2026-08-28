#!/usr/bin/env python3
"""阿波罗V2 PC-MCU 18字节头+cJSON真实TCP验收。"""

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


def frame(body: bytes, service: int = 1, command: int = 1,
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


def connect(host: str, port: int) -> socket.socket:
    connection = socket.create_connection((host, port), timeout=3.0)
    connection.settimeout(3.0)
    return connection


def expect_disconnect(host: str, port: int, payload: bytes, label: str) -> None:
    with connect(host, port) as connection:
        connection.sendall(payload)
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            try:
                if connection.recv(64) == b"":
                    return
            except (ConnectionResetError, ConnectionAbortedError, OSError):
                return
        raise AssertionError(f"{label}: MCU未按要求关闭连接")


def run(host: str, port: int) -> int:
    passed = 0

    # PC查询测试仪信息：请求包体必须为{}，响应命令改为0x0102并回显Seq。
    query_tester = frame(b"{}", service=0x1234, command=0x0101,
                         sequence=0x55667788)
    with connect(host, port) as connection:
        connection.sendall(query_tester)
        header = recv_exact(connection, HEADER_SIZE)
        fields = struct.unpack(">IHHHHIH", header)
        assert fields[1:] == (VERSION, FLAG_JSON, 0x1234, 0x0102,
                              0x55667788, 0)
        body = recv_exact(connection, fields[0] - HEADER_SIZE)
    assert json.loads(body.decode("utf-8")) == {
        "testerName": "LPDDR4/X_200B",
        "testerVersion": "D00",
        "testBoard": "RK3588_test",
    }
    print("[PASS] 0x0101查询测试仪信息、0x0102响应与Seq回显")
    passed += 1

    # PC主动心跳：MCU使用响应命令0x0001并原样回显PC请求Seq。
    heartbeat = frame(b"{}", command=0x0002, sequence=0x89ABCDEF)
    with connect(host, port) as connection:
        connection.sendall(heartbeat)
        response = recv_exact(connection, HEADER_SIZE + 2)
    heartbeat_header = struct.unpack(">IHHHHIH", response[:HEADER_SIZE])
    assert heartbeat_header == (20, VERSION, FLAG_JSON, 1, 0x0001,
                                0x89ABCDEF, 0)
    assert response[HEADER_SIZE:] == b"{}"
    print("[PASS] PC主动心跳、MCU响应命令与Seq回显")
    passed += 1

    # 半包：故意拆在Length内部、头部和UTF-8包体中，间隔小于500ms。
    body = json.dumps({"algorithmName": "algorithmName", "algorithmIndex": 1},
                      ensure_ascii=False, separators=(",", ":")).encode()
    packet = frame(body, service=0x0102, command=0x0304,
                   sequence=0x11223344)
    with connect(host, port) as connection:
        cuts = (3, 11, 19, len(packet))
        start = 0
        for end in cuts:
            connection.sendall(packet[start:end])
            start = end
            time.sleep(0.03)
        assert recv_exact(connection, len(packet)) == packet
    print("[PASS] START_TEST算法JSON任意边界半包 + PC请求Seq原样回显")
    passed += 1

    # 粘包：一个send包含两帧，服务器必须连续给出两份完整PDU。
    first = frame(body, command=0x0103, sequence=100)
    result_body = json.dumps({
        "algorithmName": "algorithmName",
        "algorithmIndex": 1,
        "testResults": [
            {"siteId": "站点1", "mirrorIndex": 1, "testStatus": 1},
            {"siteId": "", "mirrorIndex": 1, "testStatus": 1},
            {"siteId": "", "mirrorIndex": 1, "testStatus": 1},
        ],
    }, ensure_ascii=False, separators=(",", ":")).encode()
    second = frame(result_body, command=0x0105, sequence=101)
    with connect(host, port) as connection:
        connection.sendall(first + second)
        assert recv_exact(connection, len(first) + len(second)) == first + second
    print("[PASS] START_TEST + TEST_RESULT_REPORT粘包、UTF-8与连续多包")
    passed += 1

    # 已确认的MCU上限合法PDU，验证4KiB边界不是只存在于离线模型。
    max_body = b'{"pad":"' + b"x" * (MAX_PDU - HEADER_SIZE - 10) + b'"}'
    assert len(max_body) == MAX_PDU - HEADER_SIZE
    max_packet = frame(max_body, command=4, sequence=102)
    with connect(host, port) as connection:
        connection.sendall(max_packet)
        assert recv_exact(connection, MAX_PDU) == max_packet
    print("[PASS] 4096字节最大合法PDU")
    passed += 1

    invalid_cases = (
        (struct.pack(">I", HEADER_SIZE - 1), "Length小于18"),
        (struct.pack(">I", MAX_PDU + 1), "Length超过4096"),
        (frame(b"{}", version=2)[:6], "Version错误"),
        (frame(b"{}", flag=7)[:8], "Flag错误"),
        (frame(b""), "空JSON包体"),
        (frame(b'{"x":}'), "JSON语法错误"),
        (frame(b'{"x":1,"x":2}'), "JSON重复键"),
        (frame(b"[1,2,3]"), "JSON根节点不是对象"),
        (frame(b'{"ok":true}x'), "JSON尾随数据"),
        (frame(b'{"x":"\xED\xA0\x80"}'), "非法UTF-8代理区"),
        (frame(b'{"algorithmName":"algorithmName"}', command=0x0103),
         "START_TEST缺少algorithmIndex"),
        (frame(b'{"extra":1}', command=0x0002), "心跳JSON不是空对象"),
        (frame(b'{"extra":1}', command=0x0101), "测试仪信息查询JSON不是空对象"),
        (frame(b'{}', command=0x0001), "PC发送MCU方向心跳命令"),
        (frame(b'{}', command=0x0102), "PC发送MCU方向测试仪信息响应命令"),
    )
    for payload, label in invalid_cases:
        expect_disconnect(host, port, payload, label)
        print(f"[PASS] {label}主动断连")
        passed += 1

    print(f"PC protocol network: {passed} PASS, 0 FAIL")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="192.168.1.201")
    parser.add_argument("--port", type=int, default=8888)
    args = parser.parse_args()
    return run(args.host, args.port)


if __name__ == "__main__":
    raise SystemExit(main())
