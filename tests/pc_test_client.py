"""网络回归共用的PC帧客户端；只收发18字节头+JSON，不包含升级动作。"""
import json
import struct

from pc_business_network_test import make_frame, recv_exact


class Client:
    """每连接分别维护发送/接收流水号；调用者显式提供命令与预期响应。"""

    def __init__(self, connection, events):
        self.connection, self.events = connection, events
        self.tx, self.rx = 100, 1

    def call(self, body, *, command, expect, error=None):
        payload = json.dumps(body, separators=(",", ":")).encode("utf-8")
        wire = make_frame(command, payload, self.tx)
        assert len(wire) <= 4096, "请求超过MCU完整帧上限"
        self.connection.sendall(wire)
        self.tx = (self.tx + 1) & 0xFFFFFFFF
        header = struct.unpack(">IHHHHIH", recv_exact(self.connection, 18))
        assert 18 <= header[0] <= 4096, header
        assert header[1:4] == (1, 8, 0) and header[5:] == (self.rx, 0), header
        response = json.loads(recv_exact(self.connection, header[0] - 18))
        self.events.append({"command": command, "request": body,
                            "responseCommand": header[4], "mcuSequence": header[5],
                            "response": response})
        self.rx = (self.rx + 1) & 0xFFFFFFFF
        assert header[4] == expect, response
        if error:
            assert response.get("status") == "failed" and response.get("errorCode") == error, response
        return header[4], response
