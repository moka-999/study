"""普通PC测试客户端的内存收发回归，不建立网络连接。"""
import json
import struct
import unittest

from pc_business_network_test import make_frame
from pc_test_client import Client


class MemorySocket:
    def __init__(self, incoming):
        self.incoming = incoming
        self.sent = []

    def sendall(self, data):
        self.sent.append(data)

    def recv(self, size):
        result = self.incoming[:min(size, 3)]
        self.incoming = self.incoming[len(result):]
        return result


class ClientTests(unittest.TestCase):
    def test_split_receive_and_independent_sequences(self):
        response = {"status": "failed", "errorCode": "notSupported"}
        sock = MemorySocket(make_frame(1, b"{}", 1) +
                            make_frame(6, json.dumps(response).encode(), 2))
        events = []
        client = Client(sock, events)
        self.assertEqual(client.call({}, command=2, expect=1), (1, {}))
        self.assertEqual(client.call({}, command=0x901, expect=6, error="notSupported"), (6, response))
        self.assertEqual([struct.unpack(">IHHHHIH", sent[:18])[5] for sent in sock.sent], [100, 101])
        self.assertEqual([event["mcuSequence"] for event in events], [1, 2])

    def test_size_rejection_before_send(self):
        sock = MemorySocket(b"")
        with self.assertRaises(AssertionError):
            Client(sock, []).call({"x": "x" * 4096}, command=2, expect=1)
        self.assertFalse(sock.sent)


if __name__ == "__main__":
    unittest.main()
