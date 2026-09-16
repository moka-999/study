"""检查OTA设计示例和传输边界，不模拟Flash，也不宣称固件或网络已通过。"""

import base64
import binascii
import json
from pathlib import Path
import re
import struct


ROOT = Path(__file__).resolve().parents[1]
HEADER = struct.Struct(">IHHHHIH")
MAX_PACKAGE = 2097152
MAX_BLOCK = 1024
FIELDS = {
    "begin": {"action", "packageId", "packageSize"},
    "write": {"action", "packageId", "offset", "data"},
    "finish": {"action", "packageId"},
    "query": {"action"},
    "cancel": {"action", "packageId"},
}


def unique_object(pairs):
    """JSON对象键必须唯一，避免两端解析重复键时选择不同值。"""
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("重复JSON键")
        result[key] = value
    return result


def request_valid(body):
    """仅验证设计中的请求字段；状态、文件校验、擦写由未来固件测试覆盖。"""
    action = body.get("action")
    if not isinstance(action, str) or action not in FIELDS:
        return False
    allowed = FIELDS[action] | ({"packageId"} if action == "query" else set())
    if not FIELDS[action] <= body.keys() or not body.keys() <= allowed:
        return False
    if "packageId" in body and (
        not isinstance(body["packageId"], str)
        or re.fullmatch(r"[0-9A-F]{8}", body["packageId"]) is None
    ):
        return False
    if action == "begin":
        value = body["packageSize"]
        return type(value) is int and 116 <= value <= MAX_PACKAGE
    if action == "write":
        offset, encoded = body["offset"], body["data"]
        if type(offset) is not int or not 0 <= offset < MAX_PACKAGE:
            return False
        if not isinstance(encoded, str) or not 1 <= len(encoded) <= 1368:
            return False
        try:
            raw = base64.b64decode(encoded, validate=True)
        except (ValueError, binascii.Error):
            return False
        return (1 <= len(raw) <= MAX_BLOCK
                and base64.b64encode(raw).decode("ascii") == encoded)
    return True


def frame(command, sequence, body):
    """构造设计检查用的18字节大端帧，JSON长度按UTF-8字节计算。"""
    data = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return HEADER.pack(18 + len(data), 1, 8, 0, command, sequence, 0) + data


def main():
    """运行示例、反例及最大尺寸检查；失败时抛出断言并返回非零退出码。"""
    assert HEADER.size == 18
    examples = 0
    for name in ("pc_ota_protocol.md", "pc_tcp_protocol_spec.md"):
        text = (ROOT / "doc" / name).read_text(encoding="utf-8")
        for block in re.findall(r"```json\s*\n(.*?)\n```", text, re.S):
            body = json.loads(block, object_pairs_hook=unique_object)
            assert isinstance(body, dict), name
            if "action" not in body:
                continue
            if "status" not in body:
                assert request_valid(body), (name, body)
            packet = frame(0x0901, 1, body)
            assert len(packet) <= 4096, name
            if "status" in body:
                assert len(packet) <= 384, ("响应缓冲超限", name)
            examples += 1

    block = bytes(range(256)) * 4
    encoded = base64.b64encode(block).decode("ascii")
    write = {"action": "write", "packageId": "FFFFFFFF", "offset": 2096128, "data": encoded}
    assert len(encoded) == 1368 and request_valid(write)
    packet = frame(0x0901, 0xFFFFFFFF, write)
    assert HEADER.unpack(packet[:18]) == (len(packet), 1, 8, 0, 0x0901, 0xFFFFFFFF, 0)
    assert len(packet) <= 4096
    assert base64.b64decode(json.loads(packet[18:])["data"]) == block
    assert len(frame(0x0901, 0, write)) == len(packet)
    assert request_valid({"action": "query", "packageId": "00000000"})
    assert request_valid({"action": "begin", "packageId": "FFFFFFFF", "packageSize": MAX_PACKAGE})

    # 以下是字段校验反例；不以Python模型代替C下载状态机测试。
    invalid = [
        {**write, "offset": value} for value in (-1, 0.5, True, MAX_PACKAGE, 0xFFFFFFFF)
    ] + [
        {**write, "data": value} for value in (
            "", "T1RBMQ", "T1RBMR==", "T1RBMQ==\n", "T1RBMQ===", "____", "中文",
            base64.b64encode(block + b"x").decode("ascii"),
        )
    ] + [
        {**write, "packageId": value} for value in (None, 123, "5a1b2c3d", "0x12345678", "123456789")
    ] + [
        {"action": "begin", "packageId": "12345678", "packageSize": value}
        for value in (True, 115, MAX_PACKAGE + 1, "1000", 1000.5)
    ] + [
        {"action": "finish"}, {**write, "slot": "A2"}, {"action": "force"},
        {"action": []}, {"action": "query", "address": 0x08000000},
    ]
    for body in invalid:
        assert not request_valid(body), body
    try:
        json.loads('{"action":"query","action":"finish"}', object_pairs_hook=unique_object)
    except ValueError:
        pass
    else:
        raise AssertionError("重复键未拒绝")

    # 最大查询组合，包括实际版本、待安装包和下载进度，应可复用现有384字节响应缓冲。
    status = {
        "action": "query", "packageId": "FFFFFFFF", "status": "completed",
        "state": "commitUnknown", "activePackageId": "FFFFFFFF",
        "firmwareVersion": "65535.65535.65535", "confirmed": False,
        "download": {"packageId": "FFFFFFFF", "receivedBytes": MAX_PACKAGE, "totalBytes": MAX_PACKAGE},
    }
    response_bytes = len(frame(0x0902, 0xFFFFFFFF, status))
    assert response_bytes <= 384
    assert (MAX_PACKAGE - 1) + 2 > MAX_PACKAGE
    assert not 2 <= MAX_PACKAGE - (MAX_PACKAGE - 1)

    # 防止文档改回旧2048字节基线而未与下载后台同步。
    source = (ROOT / "App/ota/ota_app.c").read_text(encoding="utf-8")
    assert "bytes > 1024U" in source
    assert "0x00200000" in (ROOT / "App/ota/ota_install.h").read_text(encoding="utf-8")
    for name in ("pc_ota_protocol.md", "pc_tcp_protocol_spec.md"):
        text = (ROOT / "doc" / name).read_text(encoding="utf-8")
        for command in ("0x0901", "0x0902", "0x0903"):
            assert command in text
    print(f"PASS: {examples}个OTA JSON示例，{len(invalid) + 1}个字段反例；最大数据PDU={len(packet)}B，查询PDU={response_bytes}B")
    print("仅设计合同检查；未验证C实现、网络、Flash或实际升级。")


if __name__ == "__main__":
    main()
