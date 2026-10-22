#!/usr/bin/env python3
"""独立OTA工具回归：真实包格式、内存网络故障、双板入口及脱离仓库运行。

仅使用内存/本机回环连接和临时文件，不连接MCU、串口或ST-Link；样本由正式打包器生成。
"""
from contextlib import redirect_stdout, redirect_stderr
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import ota_client as ota
import ota_bundle
import ota_dev

spec = importlib.util.spec_from_file_location("pack_ota_reference", ROOT.parent / "bootloader/tools/pack_ota.py")
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


def sample(target="b1-h743"):
    """构造0.1.2包，最小真实复位向量及两个数据块，不包含可烧录应用。"""
    image = struct.pack("<2I", 0x20020000, ota.A1_BASE + 9) + bytes(8184)
    moved = struct.pack("<2I", 0x20020000, ota.A2_BASE + 9) + bytes(8184)
    version = bytearray(76)
    struct.pack_into("<5H", version, 0, 1, 76, 0, 1, 2)
    struct.pack_into("<2I", version, 12, len(image), zlib.crc32(image))
    version[20:44] = b"2026-10-15T00:00:00Z".ljust(24, b"\0")
    version[44:60] = b"tool-test".ljust(16, b"\0")
    version[60:76] = target.encode().ljust(16, b"\0")
    return pack.build_package(image, bytes(version), [4], moved)


class FakeSocket:
    """按真实头/JSON处理请求，响应每次只返回7字节，覆盖拆包及Seq回绕。"""

    def __init__(self, server):
        self.server, self.response, self.closed = server, b"", False
        self.sequence = 0xFFFFFFFE

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.closed = True

    def settimeout(self, timeout):
        pass

    def sendall(self, wire):
        header = ota.HEADER.unpack(wire[:18])
        if header[0] != len(wire) or len(wire) > 4096:
            raise AssertionError("错误PDU长度")
        body = json.loads(wire[18:])
        self.server.requests.append((header, body))
        command, response = self.server.reply(header[4], body)
        if response is None:
            self.response = b""
            return
        payload = json.dumps(response, separators=(",", ":")).encode()
        self.response = ota.HEADER.pack(18 + len(payload), 1, 8, 0, command, self.sequence, 0) + payload
        self.sequence = (self.sequence + 1) & 0xFFFFFFFF

    def recv(self, count):
        data, self.response = self.response[:min(count, 7)], self.response[min(count, 7):]
        return data


class FakeServer:
    """可在每个动作注入失败；只在finish已提交后模拟新版本，不虚构重刷行为。"""

    def __init__(self, package, failure=None):
        self.package, self.failure = package, failure
        self.requests, self.connections, self.written = [], [], bytearray()
        self.committed = False

    def connect(self, *args, **kwargs):
        if self.connections and not self.connections[-1].closed:
            raise AssertionError("出现第二条并发连接")
        connection = FakeSocket(self)
        self.connections.append(connection)
        return connection

    def reply(self, command, body):
        if command == 0x0503:
            return 0x0504, {"powerState": "on" if self.failure == "power" else "off"}
        if command != 0x0901:
            raise AssertionError(f"工具不应发起硬件或算法动作：{command:04X}")
        action = body["action"]
        if action not in ("mcu_query", "mcu_begin", "mcu_write", "mcu_finish"):
            raise AssertionError("不应取消事务或发送其他动作")
        response = {"action": action, "status": "completed"}
        if "packageId" in body:
            response["packageId"] = body["packageId"]
        if action == "mcu_query":
            response.update(state="confirmed", confirmed=self.failure != "health",
                            firmwareVersion="0.1.2" if self.committed or self.failure == "version" else "0.1.1")
            if self.failure == "receiving":
                response.update(state="receiving", download={"receivedBytes": 1})
            if self.committed and self.failure == "wrong-confirm":
                response["packageId"] = "00000000"
            if self.committed and self.failure == "no-confirm":
                response.update(state="trial", confirmed=False)
                return 0x0902, response
            return (0x0903 if self.committed else 0x0902), response
        if action == "mcu_begin":
            if self.failure == "busy":
                return 6, {"requestCommandId": 0x0901, "status": "failed", "errorCode": "busy"}
            response.update(state="receiving", nextOffset=1 if self.failure == "resume" else 0,
                            blockSize=3001 if self.failure == "block-size" else 3000)
        elif action == "mcu_write":
            if self.failure == "write-drop":
                return 0, None
            import base64
            offset = body["offset"]
            block = base64.b64decode(body["data"], validate=True)
            if offset != len(self.written) or block != self.package.data[offset:offset + len(block)]:
                raise AssertionError("写入的块或偏移错误")
            self.written.extend(block)
            response.update(state="receiving", offset=offset,
                            nextOffset=offset if self.failure == "offset" else len(self.written))
        else:
            if self.written != self.package.data:
                raise AssertionError("完整接收前不能提交")
            if self.failure == "finish-reject":
                return 6, {"requestCommandId": 0x0901, "status": "failed", "errorCode": "crcMismatch"}
            self.committed = True
            if self.failure == "finish-drop":
                return 0, None
            if self.failure == "commit-unknown":
                return 6, {"requestCommandId": 0x0901, "status": "failed", "errorCode": "commitUnknown"}
            response.update(state="ready", status="accepted")
        return 0x0902, response


class ToolTests(unittest.TestCase):
    def test_package_validation(self):
        for target in ota.BOARD_NAMES:
            with self.subTest(target=target):
                data = sample(target)
                result = ota.validate_package(data, target)
                self.assertEqual(result.version, (0, 1, 2))
                self.assertEqual(result.image_bytes, 8192)
                for position in (0, 12, 36, 44, 100, 116, 124, len(data) - 1):
                    bad = bytearray(data)
                    bad[position] ^= 1
                    with self.assertRaises(ValueError):
                        ota.validate_package(bad, target)
                for bad in (data[:-1], data + b"\0", bytes(ota.PACKAGE_LIMIT + 1)):
                    with self.assertRaises(ValueError):
                        ota.validate_package(bad, target)
                other = next(name for name in ota.BOARD_NAMES if name != target)
                with self.assertRaisesRegex(ValueError, "板型"):
                    ota.validate_package(data, other)

    def test_semantic_validation_even_with_correct_crcs(self):
        for kind in ("bad-offset", "bad-vector", "a2-crc", "schema", "count", "text"):
            with self.subTest(kind=kind):
                data = bytearray(sample())
                if kind == "bad-offset":
                    struct.pack_into("<I", data, 116, 0)
                elif kind == "bad-vector":
                    struct.pack_into("<I", data, 120, 0x21000000)
                elif kind == "a2-crc":
                    data[24] ^= 1
                elif kind == "schema":
                    data[40] = 2
                elif kind == "count":
                    struct.pack_into("<I", data, 16, 0xFFFFFFFF)
                else:
                    data[60:84] = b"x" * 24
                struct.pack_into("<I", data, 56, zlib.crc32(data[120:]))
                struct.pack_into("<I", data, 20, zlib.crc32(data[116:120]))
                struct.pack_into("<I", data, 28, zlib.crc32(data[40:116]))
                struct.pack_into("<I", data, 36, zlib.crc32(data[:36]))
                with self.assertRaises(ValueError):
                    ota.validate_package(data, "b1-h743")

    def run_upgrade(self, target, failure=None):
        package = ota.validate_package(sample(target), target)
        server = FakeServer(package, failure)
        events = []
        with patch.object(ota.socket, "create_connection", server.connect), \
             patch.object(ota.time, "sleep"), redirect_stdout(io.StringIO()):
            if failure in (None, "finish-drop", "commit-unknown"):
                result = ota.upgrade("unused", 8888, package, events.append)
                self.assertTrue(result["confirmed"])
            else:
                with self.assertRaises((ValueError, RuntimeError, OSError)):
                    ota.upgrade("unused", 8888, package, events.append)
        self.assertTrue(all(connection.closed for connection in server.connections))
        actions = [body.get("action") for _, body in server.requests]
        self.assertLessEqual(actions.count("mcu_finish"), 1)
        if failure in ("health", "power", "version", "receiving"):
            self.assertNotIn("mcu_begin", actions)
        if failure in ("busy", "resume", "block-size", "offset", "write-drop"):
            self.assertNotIn("mcu_finish", actions)
        if failure in ("finish-drop", "commit-unknown", "wrong-confirm"):
            self.assertEqual(actions[actions.index("mcu_finish") + 1:], ["mcu_query"])
        if failure == "finish-reject":
            self.assertEqual(actions[-1], "mcu_finish")
        self.assertTrue(all("data" not in event.get("body", {}) for event in events))

    def test_workflow_both_targets(self):
        for target in ota.BOARD_NAMES:
            for failure in (None, "health", "power", "version", "receiving", "busy", "resume", "block-size",
                            "offset", "write-drop", "finish-drop", "commit-unknown", "finish-reject", "wrong-confirm"):
                with self.subTest(target=target, failure=failure):
                    self.run_upgrade(target, failure)

    def test_query_only_and_timeout(self):
        package = ota.validate_package(sample(), "b1-h743")
        server = FakeServer(package)
        events = []
        with server.connect() as connection, redirect_stdout(io.StringIO()):
            ota.query(ota.Client(connection, events.append))
        self.assertEqual([header[4] for header, _ in server.requests], [0x0901, 0x0503])
        server.failure, server.committed = "no-confirm", True
        ticks = iter(range(10000))
        with patch.object(ota.socket, "create_connection", server.connect), patch.object(ota.time, "sleep"), \
             patch.object(ota.time, "monotonic", lambda: next(ticks)):
            with self.assertRaises(TimeoutError):
                ota.wait_confirmed("unused", 8888, package, events.append, wait_seconds=40)
        self.assertTrue(all(body.get("action") in (None, "mcu_query") for _, body in server.requests))

    def test_invalid_responses(self):
        package = ota.validate_package(sample(), "b1-h743")
        for kind in ("length", "json", "action", "seq", "direction"):
            with self.subTest(kind=kind):
                server = FakeServer(package)
                with server.connect() as connection:
                    client = ota.Client(connection, lambda event: None)
                    original = connection.sendall

                    def send(wire):
                        original(wire)
                        data = bytearray(connection.response)
                        if kind == "length":
                            struct.pack_into(">I", data, 0, 4097)
                        elif kind == "json":
                            data[-1] = 0xFF
                        elif kind == "action":
                            data = data.replace(b"mcu_query", b"mcu_begin")
                        elif kind == "seq":
                            client.rx = 9
                        else:
                            struct.pack_into(">H", data, 10, 0x0105)
                        connection.response = bytes(data)

                    connection.sendall = send
                    with self.assertRaises(ValueError):
                        client.call({"action": "mcu_query"})

    def test_portable_bundles_and_fixed_entries(self):
        with tempfile.TemporaryDirectory(prefix="ota-portable-") as temp:
            root = Path(temp)
            for target, board in (("b1-h743", "b1"), ("apollo-v2-h743", "v2")):
                path = root / f"{board}.ota"
                path.write_bytes(sample(target))
                destination = root / f"delivery-{board}-0.1.2"
                archive = ota_bundle.bundle(path, destination, target)
                self.assertEqual(archive.name, destination.name + ".zip")
                with self.assertRaises(ValueError):
                    ota_bundle.bundle(path, destination, target)
                extracted = root / f"standalone-{board}"
                with zipfile.ZipFile(archive) as package:
                    self.assertEqual(set(package.namelist()), {f"{board}_ota.py", f"start_{board}_ota.cmd",
                        f"{board}_ota_README.md", "ota_client.py", f"{board}.ota", "package_info.json"})
                    package.extractall(extracted)
                batch = (extracted / f"start_{board}_ota.cmd").read_bytes()
                self.assertIn(b"\r\n", batch)
                self.assertNotIn(b"\n", batch.replace(b"\r\n", b""))
                # 禁用用户site与Python环境路径，cwd不是源码；仅交付目录内文件参与运行。
                command = [sys.executable, "-E", "-s", "-B", "-X", "utf8", str(extracted / f"{board}_ota.py")]
                result = subprocess.run(command + ["inspect", "--package", str(extracted / f"{board}.ota")],
                                        cwd=root, capture_output=True, text=True, encoding="utf-8")
                self.assertEqual(result.returncode, 0, result.stderr)
                other = root / "wrong.ota"
                other.write_bytes(sample("apollo-v2-h743" if board == "b1" else "b1-h743"))
                result = subprocess.run(command + ["inspect", "--package", str(other)],
                                        cwd=root, capture_output=True, text=True, encoding="utf-8")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("板型", result.stderr)
                result = subprocess.run(command, input="0\n", cwd=root, capture_output=True, text=True, encoding="utf-8")
                self.assertEqual(result.returncode, 0)
                self.assertIn(board.upper(), result.stdout)

    def test_cli_requires_explicit_permission_and_target(self):
        with tempfile.TemporaryDirectory(prefix="ota-cli-") as temp, redirect_stderr(io.StringIO()):
            with patch.object(ota.socket, "create_connection") as connect:
                with self.assertRaises(SystemExit):
                    ota.main(["upgrade", "--host", "unused", "--package", "unused.ota"],
                             target="b1-h743", tool_dir=Path(temp))
                with self.assertRaises(SystemExit):
                    ota.main(["query"], target="apollo-v2-h743", tool_dir=Path(temp))
                connect.assert_not_called()

    def test_v2_build_publishes_fixed_target(self):
        with patch.object(sys, "argv", ["ota_dev.py", "build", "--version", "0.1.2", "--cmake", "cmake",
                                        "--ninja", "ninja", "--compiler", "gcc"]), \
             patch.object(ota_dev, "build_package", return_value=Path("dummy")), \
             patch.object(ota_bundle, "publish") as publish, redirect_stdout(io.StringIO()):
            ota_dev.main()
            publish.assert_called_once_with(Path("dummy/test.ota"), "apollo-v2-h743", ROOT / "build")

    def test_b1_build_publishes_fixed_target(self):
        specification = importlib.util.spec_from_file_location("b1_ota_build", ROOT.parent / "b1/tools/ota_build.py")
        builder = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(builder)
        with patch.object(sys, "argv", ["ota_build.py", "--version", "0.1.2", "--cmake", "cmake",
                                        "--ninja", "ninja", "--compiler", "gcc"]), \
             patch.object(builder.subprocess, "run") as run, patch.object(builder, "publish") as publish, \
             redirect_stdout(io.StringIO()):
            builder.main()
            self.assertEqual(run.call_count, 2)
            command = run.call_args_list[0].args[0]
            self.assertEqual(command[command.index("--app-name") + 1], "b1")
            self.assertEqual(command[command.index("--rk-uart") + 1], "4")
            self.assertNotIn("--power-simulation", command)
            publish.assert_called_once_with(builder.ROOT / "build/ota-app-0.1.2-release-a1/b1.ota",
                                            "b1-h743", builder.ROOT / "build")

    def test_loopback_upload(self):
        """只绑定127.0.0.1随机端口，走真实TCP上传/重连，不访问局域网板卡。"""
        for target in ota.BOARD_NAMES:
            with self.subTest(target=target):
                package = ota.validate_package(sample(target), target)
                backend, errors = FakeServer(package), []
                with ota.socket.socket() as listener:
                    listener.bind(("127.0.0.1", 0))
                    listener.listen(1)
                    listener.settimeout(10)

                    def serve():
                        try:
                            for _ in range(2):
                                sock, _ = listener.accept()
                                with sock:
                                    sock.settimeout(10)
                                    def exact(count):
                                        data = bytearray()
                                        while len(data) < count:
                                            block = sock.recv(count - len(data))
                                            if not block:
                                                raise EOFError()
                                            data.extend(block)
                                        return bytes(data)
                                    sequence = 1
                                    while True:
                                        try:
                                            header = ota.HEADER.unpack(exact(18))
                                        except EOFError:
                                            break
                                        body = json.loads(exact(header[0] - 18))
                                        backend.requests.append((header, body))
                                        command, response = backend.reply(header[4], body)
                                        payload = json.dumps(response, separators=(",", ":")).encode()
                                        sock.sendall(ota.HEADER.pack(18 + len(payload), 1, 8, 0, command, sequence, 0) + payload)
                                        sequence += 1
                        except Exception as error:
                            errors.append(error)

                    worker = threading.Thread(target=serve, daemon=True)
                    worker.start()
                    try:
                        with redirect_stdout(io.StringIO()):
                            result = ota.upgrade("127.0.0.1", listener.getsockname()[1], package, lambda event: None)
                        self.assertEqual(result["packageId"], package.package_id)
                    finally:
                        worker.join(timeout=12)
                    self.assertFalse(worker.is_alive())
                    self.assertEqual(errors, [])
                self.assertEqual(backend.written, package.data)


if __name__ == "__main__":
    unittest.main(verbosity=2)
