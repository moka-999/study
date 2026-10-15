#!/usr/bin/env python3
"""仅自测网络验收脚本，不连接板卡；固定响应不是MCU固件验证。"""

import contextlib
import io
import json
import socket
import struct
import unittest
from unittest.mock import patch

import pc_protocol_network_test as network
from verify_pc_protocol import StreamModel


class FakeSocket:
    """记录发送，按最多7字节拆分固定响应，覆盖脚本自身的分段接收。"""

    def __init__(self, response=b"", error=None):
        self.response = bytearray(response)
        self.error = error
        self.sent = bytearray()
        self.closed = False

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.closed = True

    def sendall(self, data):
        self.sent.extend(data)

    def recv(self, length):
        if self.error:
            raise self.error
        count = min(length, 7, len(self.response))
        result = bytes(self.response[:count])
        del self.response[:count]
        return result


def response(command, sequence, body):
    data = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode()
    return struct.pack(">IHHHHIH", 18 + len(data), 1, 8, 0, command, sequence, 0) + data


class NetworkScriptTests(unittest.TestCase):
    def test_complete_run_uses_safe_requests_and_independent_sequences(self):
        # 固定响应清单独立于脚本的收发函数，不实现第二套业务分发器。
        replies = [(0x0102, {"testerName": "JMCT-4000", "testerVersion": "D00",
                              "testBoard": "RK3588_test"}), (1, {})]
        replies.append((6, {"requestCommandId": 0x7FFF, "status": "failed",
                            "errorCode": "notSupported"}))
        replies.extend([(1, {})] * 9)  # 两帧粘包、4KiB以及六种坏前缀。
        for command in (2, 2, 2, 2, 2, 2, 0x0103, 2, 0x0101):
            replies.extend([(6, {"requestCommandId": command, "status": "failed",
                                  "errorCode": "invalidArgument"}), (1, {})])
        for command in (1, 0x0102, 0x0105):
            replies.append((6, {"requestCommandId": command, "status": "failed",
                                "errorCode": "invalidDirection"}))
        stream = b"".join(response(command, seq, body)
                          for seq, (command, body) in enumerate(replies, 1))
        connections = [FakeSocket(stream), FakeSocket(), FakeSocket(),
                       FakeSocket(response(1, 1, {}))]
        opened = []

        def connect(_host, _port):
            if opened:
                self.assertTrue(opened[-1].closed, "不能同时占用两个TCP连接")
            connection = connections[len(opened)]
            opened.append(connection)
            return connection

        with patch.object(network, "connect", side_effect=connect), \
                patch.object(network.time, "sleep"), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(network.run("unused.invalid", 0), 0)
        self.assertIn("26 PASS, 0 FAIL", output.getvalue())
        self.assertEqual(len(opened), 4)
        self.assertTrue(all(item.closed and not item.response for item in connections))

        # 复用现有流模型只拆检查脚本发送内容，不能据此宣称实际固件恢复通过。
        model, requests = StreamModel(), []
        for value in connections[0].sent:
            packet = model.push(value)
            if packet is not None:
                fields = struct.unpack(">IHHHHIH", packet[:18])
                requests.append((fields, packet[18:]))
                model.reset()
        self.assertEqual(len(requests), 33)
        self.assertEqual([fields[5] for fields, _ in requests],
                         list(range(0x55667788, 0x55667788 + 33)))
        self.assertTrue(all(fields[3] == 0 for fields, _ in requests))
        self.assertEqual({fields[4] for fields, _ in requests},
                         {1, 2, 0x0101, 0x0102, 0x0103, 0x0105, 0x7FFF})
        self.assertEqual([body for fields, body in requests if fields[4] == 0x0103],
                         [b'{"algorithmName":"mats"}'])
        self.assertEqual(len(connections[1].sent), 10)
        self.assertEqual(connections[2].sent, b"T" * 4097)
        self.assertEqual(struct.unpack(">IHHHHIH", connections[3].sent[:18])[5], 900)

    def test_response_validation_rejects_bad_header_json_and_short_read(self):
        good = response(1, 7, {})
        network.expect_response(FakeSocket(good), 1, 7, {})
        for index, value in ((0, 17), (0, 4097), (1, 2), (2, 7), (3, 1),
                             (4, 2), (5, 8), (6, 1)):
            fields = list(struct.unpack(">IHHHHIH", good[:18]))
            fields[index] = value
            invalid = struct.pack(">IHHHHIH", *fields) + good[18:]
            with self.subTest(field=index, value=value), self.assertRaises(AssertionError):
                network.expect_response(FakeSocket(invalid), 1, 7, {})
        for invalid in (good[:-1], good[:-2], response(1, 7, {"wrong": 1})):
            with self.subTest(packet=invalid), self.assertRaises(AssertionError):
                network.expect_response(FakeSocket(invalid), 1, 7, {})
        with self.assertRaises(json.JSONDecodeError):
            network.expect_response(FakeSocket(good[:18] + b"xx"), 1, 7, {})

    def test_disconnect_requires_eof_or_explicit_connection_close(self):
        for error in (None, ConnectionResetError(), ConnectionAbortedError()):
            with self.subTest(error=error), patch.object(network, "connect",
                                                        return_value=FakeSocket(error=error)):
                network.expect_disconnect("unused.invalid", 0, b"x", "断连测试")
        for error, expected in ((socket.timeout(), AssertionError),
                                (OSError("未知系统错误"), OSError)):
            with self.subTest(error=error), patch.object(network, "connect",
                    return_value=FakeSocket(error=error)), self.assertRaises(expected):
                network.expect_disconnect("unused.invalid", 0, b"x", "断连测试")
        with patch.object(network, "connect", return_value=FakeSocket(b"x")), \
                self.assertRaises(AssertionError):
            network.expect_disconnect("unused.invalid", 0, b"x", "断连测试")


if __name__ == "__main__":
    unittest.main()
