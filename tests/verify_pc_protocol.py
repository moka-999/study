#!/usr/bin/env python3
"""PC-MCU 固定头/JSON框架的离线合同测试，不依赖板卡和网络。"""

from __future__ import annotations

import hashlib
import json
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER_SIZE = 18
MAX_PDU = 4096
VERSION = 1
FLAG_JSON = 8


class ProtocolError(ValueError):
    """表示固件应关闭当前 TCP 连接的致命组帧错误。"""


class StreamModel:
    """按 C 模块合同建立的最小字节流模型，用于组合半包和粘包场景。"""

    def __init__(self) -> None:
        self.buffer = bytearray()
        self.expected = 0

    def reset(self) -> None:
        self.buffer.clear()
        self.expected = 0

    def push(self, value: int) -> bytes | None:
        self.buffer.append(value)
        if len(self.buffer) == 4:
            self.expected = struct.unpack_from(">I", self.buffer)[0]
            if not HEADER_SIZE <= self.expected <= MAX_PDU:
                raise ProtocolError("length")
        if len(self.buffer) == 6 and struct.unpack_from(">H", self.buffer, 4)[0] != VERSION:
            raise ProtocolError("version")
        if len(self.buffer) == 8 and struct.unpack_from(">H", self.buffer, 6)[0] != FLAG_JSON:
            raise ProtocolError("flag")
        if self.expected and len(self.buffer) == self.expected:
            return bytes(self.buffer)
        return None


def build_frame(body: bytes, service: int, command: int, sequence: int) -> bytes:
    length = HEADER_SIZE + len(body)
    return struct.pack(">IHHHHIH", length, VERSION, FLAG_JSON,
                       service, command, sequence, 0) + body


def expect_protocol_error(data: bytes, label: str) -> None:
    stream = StreamModel()
    try:
        for value in data:
            stream.push(value)
    except ProtocolError:
        return
    raise AssertionError(f"未拒绝非法{label}")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def main() -> None:
    passed = 0

    body_a = json.dumps({"board": "B1", "ip": "192.168.1.201"},
                        ensure_ascii=False, separators=(",", ":")).encode()
    body_b = "{\"message\":\"中文UTF-8\"}".encode()
    frame_a = build_frame(body_a, 0x0102, 0x0304, 0x11223344)
    frame_b = build_frame(body_b, 0x0102, 0x0305, 0x11223345)

    # 每字节一次送入，覆盖任意位置半包。
    stream = StreamModel()
    completed = [stream.push(value) for value in frame_a]
    assert completed[:-1] == [None] * (len(frame_a) - 1)
    assert completed[-1] == frame_a
    passed += 1

    # 同一 recv 缓冲可包含连续两帧；处理完第一帧 reset 后继续消费余下字节。
    decoded: list[bytes] = []
    stream.reset()
    for value in frame_a + frame_b:
        result = stream.push(value)
        if result is not None:
            decoded.append(result)
            stream.reset()
    assert decoded == [frame_a, frame_b]
    passed += 1

    fields = struct.unpack(">IHHHHIH", decoded[0][:HEADER_SIZE])
    assert fields == (len(frame_a), 1, 8, 0x0102, 0x0304, 0x11223344, 0)
    assert json.loads(decoded[0][HEADER_SIZE:].decode("utf-8"))["board"] == "B1"
    passed += 1

    expect_protocol_error(struct.pack(">I", 17), "最小Length")
    expect_protocol_error(struct.pack(">I", MAX_PDU + 1), "最大Length")
    expect_protocol_error(struct.pack(">IH", HEADER_SIZE, 2), "Version")
    expect_protocol_error(struct.pack(">IHH", HEADER_SIZE, VERSION, 7), "Flag")
    passed += 4

    # 固定依赖版本和文件摘要，防止无意中升级或改写第三方源码。
    vendor = ROOT / "Middlewares" / "Third_Party" / "cJSON"
    header_text = (vendor / "cJSON.h").read_text(encoding="utf-8")
    assert "#define CJSON_VERSION_MAJOR 1" in header_text
    assert "#define CJSON_VERSION_MINOR 7" in header_text
    assert "#define CJSON_VERSION_PATCH 19" in header_text
    assert sha256(vendor / "cJSON.c") == "298581A04A36C0165DA4B0AADE235C23088CB2FAA58651D720EA2F3706ED0B0D"
    assert sha256(vendor / "cJSON.h") == "25B0145150D500498E4D209CEC69C18C42CF818BFFCC54690BE3B895A2A16DEE"
    passed += 1

    cmake_text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "App/protocol/pc/pc_protocol.c" in cmake_text
    assert "App/protocol/rk/frame.c" in cmake_text
    assert "Middlewares/Third_Party/cJSON/cJSON.c" in cmake_text
    assert "CJSON_NESTING_LIMIT=16" in cmake_text
    passed += 1

    source = (ROOT / "App" / "protocol" / "pc" / "pc_protocol.c").read_text(
        encoding="utf-8")
    assert "cJSON_ParseWithLengthOpts" in source
    assert "PC_JSON_ERROR_DUPLICATE_KEY" in source
    assert "utf8_is_valid" in source
    passed += 1

    # 正式TCP任务必须使用PC解析器，不能再把PC输入送入RK TLV状态机或原样透传。
    tcp_source = (ROOT / "App" / "tasks" / "pc" / "tcp_task.c").read_text(
        encoding="utf-8")
    assert "pc_protocol_stream_push_byte" in tcp_source
    assert "pc_validate_business_json" in tcp_source
    assert "pc_send_heartbeat_response" in tcp_source
    assert "tlv_sm_feed" not in tcp_source
    assert "rkToPc_Queue" not in tcp_source
    passed += 1

    command_header = (ROOT / "App" / "protocol" / "pc" /
                      "pc_command.h").read_text(encoding="utf-8")
    assert "PC_COMMAND_SERVER_HEARTBEAT   0x0001U" in command_header
    assert "PC_COMMAND_CLIENT_HEARTBEAT   0x0002U" in command_header
    passed += 1

    request = json.loads('{"algorithmName":"algorithmName","algorithmIndex":1}')
    result = json.loads(
        '{"algorithmName":"algorithmName","algorithmIndex":1,'
        '"testResults":[{"siteId":"","mirrorIndex":1,"testStatus":1}]}'
    )
    assert isinstance(request["algorithmName"], str)
    assert isinstance(request["algorithmIndex"], int)
    assert isinstance(result["testResults"], list)
    try:
        json.loads('{"algorithmName":"algorithmName","algorithmIndex":1,}')
    except json.JSONDecodeError:
        pass
    else:
        raise AssertionError("算法请求尾随逗号必须被拒绝")
    business_source = (ROOT / "App" / "protocol" / "pc" /
                       "pc_test_message.c").read_text(encoding="utf-8")
    assert '"algorithmName"' in business_source
    assert '"algorithmIndex"' in business_source
    assert '"testResults"' in business_source
    passed += 1

    # MCU响应必须回显PC请求Seq；RK的u64 Seq不参与PC头部组帧。
    response = build_frame(result_body := json.dumps(
        result, ensure_ascii=False, separators=(",", ":")).encode(),
        service=0x0102, command=0x0105, sequence=0x11223344)
    response_fields = struct.unpack(">IHHHHIH", response[:HEADER_SIZE])
    assert response_fields[4] == 0x0105
    assert response_fields[5] == fields[5] == 0x11223344
    assert json.loads(result_body.decode("utf-8"))["algorithmIndex"] == 1
    passed += 1

    print(f"PC protocol contract: {passed} PASS, 0 FAIL")


if __name__ == "__main__":
    main()
