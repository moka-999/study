#!/usr/bin/env python3
"""PC-MCU 固定头/JSON框架的离线合同测试，不依赖板卡和网络。"""

from __future__ import annotations

import hashlib
import json
import re
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
        if len(self.buffer) == 10 and struct.unpack_from(">H", self.buffer, 8)[0] != 0:
            raise ProtocolError("service")
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
    frame_a = build_frame(body_a, 0, 0x0304, 0x11223344)
    frame_b = build_frame(body_b, 0, 0x0305, 0x11223345)

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

    # 帧层允许Length=18，空正文是否符合JSON由dispatch决定；不要在
    # HEADER->BODY重构时丢掉直接HEADER->FRAME_READY的边界路径。
    header_only = build_frame(b"", 0, 2, 7)
    stream.reset()
    completed = [stream.push(value) for value in header_only]
    assert completed == [None] * (HEADER_SIZE - 1) + [header_only]
    passed += 1

    # 4KiB恰好满帧仍合法，不能在最后一个字节写入前提前触发容量拒绝。
    full_frame = build_frame(b"{}" + b" " * (MAX_PDU - HEADER_SIZE - 2), 0, 2, 8)
    stream.reset()
    completed = [stream.push(value) for value in full_frame]
    assert completed[:-1] == [None] * (MAX_PDU - 1)
    assert completed[-1] == full_frame and len(full_frame) == MAX_PDU
    passed += 1

    fields = struct.unpack(">IHHHHIH", decoded[0][:HEADER_SIZE])
    assert fields == (len(frame_a), 1, 8, 0, 0x0304, 0x11223344, 0)
    assert json.loads(decoded[0][HEADER_SIZE:].decode("utf-8"))["board"] == "B1"
    passed += 1

    expect_protocol_error(struct.pack(">I", 17), "最小Length")
    expect_protocol_error(struct.pack(">I", MAX_PDU + 1), "最大Length")
    expect_protocol_error(struct.pack(">IH", HEADER_SIZE, 2), "Version")
    expect_protocol_error(struct.pack(">IHH", HEADER_SIZE, VERSION, 7), "Flag")
    expect_protocol_error(struct.pack(">IHHH", HEADER_SIZE, VERSION, FLAG_JSON, 1),
                          "ServiceID")
    passed += 5

    # 字段刚到齐时就拒绝，不因拆分状态处理函数而延迟到整帧结束。
    for invalid, reject_at in ((struct.pack(">I", MAX_PDU + 1), 4),
                               (struct.pack(">IH", 20, 2), 6),
                               (struct.pack(">IHH", 20, VERSION, 7), 8)):
        stream.reset()
        rejected_at = None
        for index, value in enumerate(invalid, 1):
            try:
                stream.push(value)
            except ProtocolError:
                rejected_at = index
                break
        assert rejected_at == reject_at
    passed += 1

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
    assert "App/protocol/pc/pc_dispatch.c" in cmake_text
    assert "App/protocol/pc/pc_protocol.c" in cmake_text
    assert "App/protocol/pc/pc_state_machine.c" in cmake_text
    assert "App/protocol/pc/pc_tester_info.c" in cmake_text
    assert "App/protocol/rk/frame.c" in cmake_text
    assert "Middlewares/Third_Party/cJSON/cJSON.c" in cmake_text
    assert "CJSON_NESTING_LIMIT=16" in cmake_text
    passed += 1

    source = (ROOT / "App" / "protocol" / "pc" / "pc_protocol.c").read_text(
        encoding="utf-8")
    assert "cJSON_ParseWithLengthOpts" in source
    assert "PC_JSON_ERROR_DUPLICATE_KEY" in source
    assert "utf8_is_valid" in source
    assert "json_string_lexemes_are_safe" in source
    passed += 1

    # PC字节流状态机必须独立于JSON/组帧模块，正式任务只调用状态机API。
    state_source = (ROOT / "App" / "protocol" / "pc" /
                    "pc_state_machine.c").read_text(encoding="utf-8")
    state_header = (ROOT / "App" / "protocol" / "pc" /
                    "pc_state_machine.h").read_text(encoding="utf-8")
    assert "PC_SM_STATE_HEADER" in state_header
    assert "PC_SM_STATE_BODY" in state_header
    assert "PC_SM_STATE_FRAME_READY" in state_header
    assert "PC_SM_STATE_ERROR" in state_header
    assert "pc_sm_feed" in state_source
    assert "pc_json_parse_object" not in state_source
    assert "PC_COMMAND_" not in state_source
    passed += 1

    # 帧状态与业务命令使用两张不同的表。此处检查实际C源码路由，
    # 不将Python模型或静态检查当成C运行；终态/非法状态运行例在Mock中。
    state_code = re.sub(r"/\*.*?\*/|//[^\n]*", "", state_source, flags=re.S)
    assert not re.search(r"\bswitch\s*\(", state_code)
    state_table = re.search(
        r"static const pc_sm_state_handler_t s_state_handlers\[\]\s*=\s*\{(.*?)\n\};",
        state_code, re.S)
    assert state_table is not None
    state_entries = re.findall(r"\[(PC_SM_STATE_\w+)\]\s*=\s*(\w+)",
                               state_table.group(1))
    assert len(state_entries) == 4
    assert dict(state_entries) == {
        "PC_SM_STATE_HEADER": "feed_header",
        "PC_SM_STATE_BODY": "feed_body",
        "PC_SM_STATE_FRAME_READY": "reject_byte",
        "PC_SM_STATE_ERROR": "reject_byte",
    }
    feed_code = state_code.split("pc_sm_feed(pc_sm_t *sm, uint8_t byte)", 1)[1]
    feed_code = feed_code.split("int pc_sm_get_frame", 1)[0]
    assert feed_code.index("(unsigned int)sm->state >= PC_SM_STATE_COUNT") < \
        feed_code.index("s_state_handlers[sm->state]")
    assert "return handler(sm, byte);" in feed_code
    assert "sm->buffer[" not in feed_code  # 只有可接收状态的handler才能写入。
    assert "_Static_assert" in state_code and "PC_SM_STATE_COUNT" in state_header
    passed += 1

    # JSON解析、命令校验和响应组帧统一由dispatch承担，不泄漏到TCP任务。
    dispatch_source = (ROOT / "App" / "protocol" / "pc" /
                       "pc_dispatch.c").read_text(encoding="utf-8")
    assert "pc_json_parse_object" in dispatch_source
    assert "pc_protocol_build_response" in dispatch_source
    assert '"requestCommandId"' in dispatch_source
    assert '"commandId"' not in dispatch_source
    assert "PC_COMMAND_QUERY_TESTER_INFO" in dispatch_source
    assert "pc_tester_info_load" in dispatch_source
    assert '"testerName"' in dispatch_source
    assert "firmware_version_current" not in dispatch_source
    passed += 1

    # 命令表取代switch分支；检查已有路由不丢失、命令号不重复。
    # 这里只检查源码合同，实际处理函数的运行回归在driver_mock_test.c中。
    assert not re.search(r"\bswitch\s*\(", dispatch_source)
    table_match = re.search(
        r"static const pc_command_entry_t s_command_table\[\]\s*=\s*\{(.*?)\n\};",
        dispatch_source, re.S)
    assert table_match is not None
    entries = re.findall(r"\{\s*(PC_COMMAND_\w+)\s*,\s*(\w+)\s*\}",
                         table_match.group(1))
    routes = dict(entries)
    assert len(routes) == len(entries), "PC命令表出现重复CommandID名称"
    command_source = (ROOT / "App" / "protocol" / "pc" /
                      "pc_command.h").read_text(encoding="utf-8")
    command_values = dict(re.findall(
        r"#define\s+(PC_COMMAND_\w+)\s+(0x[0-9A-Fa-f]+)U", command_source))
    values = [int(command_values[command], 16) for command in routes]
    assert len(set(values)) == len(values), "PC命令表出现相同数值的命令别名"
    assert routes["PC_COMMAND_CLIENT_HEARTBEAT"] == "handle_client_heartbeat"
    assert routes["PC_COMMAND_QUERY_TESTER_INFO"] == "handle_query_tester_info"
    assert routes["PC_COMMAND_START_TEST"] == "handle_start_test"
    assert routes["PC_COMMAND_SERVER_HEARTBEAT"] == "reject_response_command"
    assert routes["PC_COMMAND_TESTER_INFO_RESP"] == "reject_response_command"
    assert routes["PC_COMMAND_SET_DDR_CONFIG"] == "handle_set_ddr_config"
    assert routes["PC_COMMAND_POWER_ON"] == "handle_power_on"
    assert routes["PC_COMMAND_POWER_OFF"] == "handle_power_off"
    assert routes["PC_COMMAND_RESET_RK"] == "handle_reset_rk"
    assert routes["PC_COMMAND_SET_BOOT_MODE"] == "handle_set_boot_mode"
    assert routes["PC_COMMAND_QUERY_BOOT_MODE"] == "handle_query_boot_mode"
    assert routes["PC_COMMAND_BOOT_MODE_RESP"] == "reject_response_command"
    assert routes["PC_COMMAND_QUERY_POWER_MONITOR"] == "handle_query_power_monitor"
    assert routes["PC_COMMAND_SET_RESULT_INDICATOR"] == "handle_set_result_indicator"
    assert routes["PC_COMMAND_QUERY_INDICATOR_STATUS"] == "handle_query_indicator_status"
    assert "s_command_table[i].handler(request, mcu_tx_sequence" in dispatch_source
    passed += 1

    tester_store_source = (ROOT / "App" / "protocol" / "pc" /
                           "pc_tester_info.c").read_text(encoding="utf-8")
    assert 'TESTER_PRODUCT_NAME        "LPDDR4/X_200B"' in tester_store_source
    assert 'TESTER_HARDWARE_REVISION   "D00"' in tester_store_source
    assert 'TESTER_BOARD_NAME          "RK3588_test"' in tester_store_source
    assert "fru_info_load" in tester_store_source
    assert "fru_info_load_legacy_v1" in tester_store_source
    assert "fru_info_save" in tester_store_source
    assert "if (load_result == FLASH_STORE_OK) return 0;" in tester_store_source
    passed += 1

    # 正式TCP任务只保留socket与状态机编排，不能再包含JSON/业务处理细节。
    tcp_source = (ROOT / "App" / "tasks" / "pc" / "tcp_task.c").read_text(
        encoding="utf-8")
    assert "pc_sm_init" in tcp_source
    assert "pc_sm_feed" in tcp_source
    assert "pc_sm_get_frame" in tcp_source
    assert "pc_dispatch_handle" in tcp_source
    assert "pc_json_parse_object" not in tcp_source
    assert "pc_protocol_build_response" not in tcp_source
    assert "pc_test_start_parse" not in tcp_source
    assert "PC_COMMAND_" not in tcp_source
    assert "tlv_sm_feed" not in tcp_source
    assert "rkToPc_Queue" not in tcp_source
    passed += 1

    command_header = (ROOT / "App" / "protocol" / "pc" /
                      "pc_command.h").read_text(encoding="utf-8")
    assert "PC_COMMAND_SERVER_HEARTBEAT   0x0001U" in command_header
    assert "PC_COMMAND_CLIENT_HEARTBEAT   0x0002U" in command_header
    assert "PC_COMMAND_QUERY_TESTER_INFO  0x0101U" in command_header
    assert "PC_COMMAND_TESTER_INFO_RESP   0x0102U" in command_header
    assert "PC_COMMAND_SET_DDR_CONFIG      0x0509U" in command_header
    assert "PC_COMMAND_POWER_ON            0x0501U" in command_header
    assert "PC_COMMAND_POWER_OFF           0x0502U" in command_header
    assert "PC_COMMAND_RESET_RK            0x0505U" in command_header
    assert "PC_COMMAND_SET_BOOT_MODE       0x0506U" in command_header
    assert "PC_COMMAND_QUERY_BOOT_MODE     0x0507U" in command_header
    assert "PC_COMMAND_BOOT_MODE_RESP      0x0508U" in command_header
    assert "PC_COMMAND_QUERY_POWER_MONITOR 0x0601U" in command_header
    assert "PC_COMMAND_SET_RESULT_INDICATOR    0x0701U" in command_header
    passed += 1

    # 测试仪信息必须由0x0101查询触发，不允许连接建立后主动推送。
    tester_info = {
        "testerName": "LPDDR4/X_200B",
        "testerVersion": "D00",
        "testBoard": "RK3588_test",
    }
    tester_response = build_frame(
        json.dumps(tester_info, separators=(",", ":")).encode(),
        service=0, command=0x0102, sequence=1)
    tester_fields = struct.unpack(">IHHHHIH", tester_response[:HEADER_SIZE])
    assert tester_fields[3:] == (0, 0x0102, 1, 0)
    assert json.loads(tester_response[HEADER_SIZE:]) == tester_info
    accept_block = tcp_source.split("accept(listenfd, NULL, NULL);", 1)[1]
    before_recv = accept_block.split("recv(connfd, rx, sizeof(rx), 0);", 1)[0]
    assert "pc_dispatch_handle" not in before_recv
    passed += 1

    request = json.loads('{"algorithmName":"algorithmName","algorithmIndex":1}')
    result = json.loads(
        '{"algorithmName":"algorithmName","algorithmIndex":1,'
        '"testResults":[{"siteId":"","mirrorIndex":1,"resultDesc":"","testStatus":1}]}'
    )
    assert isinstance(request["algorithmName"], str)
    assert isinstance(request["algorithmIndex"], int)
    assert isinstance(result["testResults"], list)
    assert result["testResults"][0]["resultDesc"] == ""
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
    assert '"resultDesc"' in business_source
    assert all(f'"{field}"' in business_source for field in (
        "algorithmType", "power", "reboot", "bootMode", "memoryType",
        "vdd1Steps", "vdd2Steps", "vddqSteps"))
    assert "ddr_power_build_step_targets" in business_source
    assert "isfinite" in business_source and "pc_test_start_read_index" in business_source
    start_handler = dispatch_source.split("static pc_dispatch_result_t handle_start_test(", 1)[1].split("\n}", 1)[0]
    assert '"notReady"' in start_handler and '"otaNotReady"' in start_handler
    assert "PC_DISPATCH_NO_RESPONSE" not in start_handler
    assert "build_command_failure_with_index" in start_handler
    passed += 1

    # MCU响应使用本连接自己的Seq；PC请求Seq和RK u64 Seq都不参与响应头。
    response = build_frame(result_body := json.dumps(
        result, ensure_ascii=False, separators=(",", ":")).encode(),
        service=0, command=0x0105, sequence=1)
    response_fields = struct.unpack(">IHHHHIH", response[:HEADER_SIZE])
    assert response_fields[4] == 0x0105
    assert response_fields[3] == 0 and response_fields[5] == 1
    assert "write_be16(&output[8], 0U);" in source
    assert "write_be32(&output[12], mcu_tx_sequence);" in source
    assert "request->sequence" not in source
    assert json.loads(result_body.decode("utf-8"))["algorithmIndex"] == 1
    passed += 1

    print(f"PC protocol contract: {passed} PASS, 0 FAIL")


if __name__ == "__main__":
    main()
