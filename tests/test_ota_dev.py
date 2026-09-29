#!/usr/bin/env python3
"""日常下载入口的离线回归：版本/槽保护、步骤顺序、失败不提交及释放自有进程。

不访问网络、ST-Link或开发板；运行：python -B -X utf8 tests/test_ota_dev.py。
"""
from argparse import Namespace
import base64
from contextlib import ExitStack
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import ota_dev as dev
import pc_ota_network_test as net


def sample(active=0):
    """最小有效样本；current为0.1.8，包为0.1.9，两种活动槽都覆盖。"""
    image = struct.pack("<4I", 0x20020000, dev.SLOTS[active] + 9, 0, 0)
    version = bytearray(76)
    struct.pack_into("<5H", version, 0, 1, 76, 0, 1, 8)
    struct.pack_into("<2I", version, 12, len(image), zlib.crc32(image))
    version[60:76] = b"apollo-v2-h743".ljust(16, b"\0")
    package = bytearray(116) + image
    package[:4] = b"OTA1"
    struct.pack_into("<I", package, 12, len(package))
    struct.pack_into("<I", package, 36, zlib.crc32(package[:36]))
    package[40:116] = version
    struct.pack_into("<H", package, 48, 9)
    record = bytearray(236)
    struct.pack_into("<13I", record, 0, 1, 1, active, 0xFFFFFFFF, 1 << active,
                     0, 0, 0, 0, len(package), zlib.crc32(package[:36]), 10, 0)
    offset = 52 + active * 92
    struct.pack_into("<4I", record, offset, dev.SLOTS[active], len(image), zlib.crc32(image), 0)
    record[offset + 16:offset + 92] = version
    internal = bytearray(b"\xff" * 0x200000)
    start = dev.SLOTS[active] - 0x08000000
    internal[start:start + len(image)] = image
    return record, internal, package


def rejected(call):
    try:
        call()
    except ValueError:
        return
    raise AssertionError("非法输入未被拒绝")


def check_validation():
    for active in (0, 1):
        record, original, package = sample(active)
        assert dev.validate_request(record, original, package) == 1 - active
        for word, bad in ((1, 0), (2, 2), (4, 0), (5, 1), (6, 2), (8, 1), (9, 1), (10, 0)):
            broken = bytearray(record)
            struct.pack_into("<I", broken, word * 4, bad)
            rejected(lambda: dev.validate_request(broken, original, package))
        for patch_version in (7, 8):
            broken = bytearray(package)
            struct.pack_into("<H", broken, 48, patch_version)
            rejected(lambda: dev.validate_request(record, original, broken))
        for offset in (0, 36, 100):
            broken = bytearray(package)
            broken[offset] ^= 1
            rejected(lambda: dev.validate_request(record, original, broken))
        broken = bytearray(original)
        broken[dev.SLOTS[active] - 0x08000000] ^= 1
        rejected(lambda: dev.validate_request(record, broken, package))
        rejected(lambda: dev.validate_request(record, original[:-1], package))
        rejected(lambda: dev.validate_request(record, original, bytes(0x200001)))

        target = 1 - active
        image = b"new app test"
        installed = bytearray(original)
        start = dev.SLOTS[target] - 0x08000000
        installed[start:start + len(image)] = image
        diag = struct.pack("<8I", 0x4F544131, dev.SLOTS[target], 0, 11, 15, 30001, 0, 0)
        vtor = struct.pack("<I", dev.SLOTS[target])
        # 新四任务与旧五任务都可验证；不能把缺RK的位图7/23当成已健康。
        for mask in (15, 31):
            compatible = bytearray(diag)
            struct.pack_into("<I", compatible, 16, mask)
            dev.validate_running(installed, original, image, target, compatible, vtor)
        for offset in (0, dev.SLOTS[active] - 0x08000000, 0x1E0000, start):
            broken = bytearray(installed)
            broken[offset] ^= 1
            rejected(lambda: dev.validate_running(broken, original, image, target, diag, vtor))
        for word, bad in ((0, 0), (1, dev.SLOTS[active]), (2, 1), (4, 1), (4, 7), (4, 23), (5, 0)):
            broken = bytearray(diag)
            struct.pack_into("<I", broken, word * 4, bad)
            rejected(lambda: dev.validate_running(installed, original, image, target, broken, vtor))
    print("PASS：双槽预检、版本/CRC/板型、健康确认及非目标区域保护")


def check_workflow(fail_at=None):
    """替换硬件边界；主编排本身真实运行，确认任何失败都会释放自有服务器。"""
    with tempfile.TemporaryDirectory(prefix="ota-dev-test-") as temporary, ExitStack() as stack:
        directory = Path(temporary)
        build = directory / "ota-app-0.1.9-debug-a1"
        build.mkdir()
        record, internal, package = sample()
        (build / "test.ota").write_bytes(package)
        (build / "a2_from_a1.bin").write_bytes(b"new app test")
        steps = []
        process = SimpleNamespace(stopped=False)
        process.poll = lambda: 0 if process.stopped else None
        process.terminate = lambda: setattr(process, "stopped", True)
        process.kill = process.terminate
        process.wait = lambda timeout: 0

        def start(*args, **kwargs):
            kwargs["stdout"].write("Waiting for debugger connection\n")
            kwargs["stdout"].flush()
            return process

        def run_script(output, name, commands, **kwargs):
            steps.append(name)
            assert "load" not in commands, "编排不可通过GDB load覆盖App/B"
            if name == fail_at:
                raise RuntimeError("注入GDB失败")
            if name == "internal-backup":
                (directory / "original-internal.bin").write_bytes(internal)
            if name.startswith("health-") and fail_at != "timeout":
                (directory / "vtor.bin").write_bytes(struct.pack("<I", dev.SLOTS[1]))
                (directory / "health.bin").write_bytes(struct.pack("<8I", 0x4F544131, dev.SLOTS[1], 0, 11, 15, 30001, 0, 0))
                installed = bytearray(internal)
                installed[0x100000:0x10000C] = b"new app test"
                (directory / "installed-internal.bin").write_bytes(installed)

        def action(argv):
            steps.append(argv[0])
            if argv[0] == fail_at:
                raise RuntimeError("注入失败")
            if argv[0] == "write":
                assert argv[argv.index("--address") + 1] == "0x10000"
                assert steps.index("backup") < steps.index("write")
            if argv[0] == "read":
                data = (directory / "upgrade-package-4k.bin").read_bytes()
                (directory / "stage-readback.bin").write_bytes(data if fail_at != "readback" else b"bad")
            if argv[0] == "record":
                assert steps.index("read") < steps.index("record")

        def run(*command):
            if any(str(part).endswith("prepare_board_ota.py") for part in command):
                steps.append("prepare")
                assert "--journal" in command
                (directory / "upgrade-record.bin").write_bytes(record)
                (directory / "upgrade-package-4k.bin").write_bytes(package + b"\xff" * (-len(package) % 4096))

        board = SimpleNamespace(run_script=run_script, main=action)
        stack.enter_context(patch.dict(sys.modules, {"board_ota": board}))
        stack.enter_context(patch.object(dev, "run", run))
        stack.enter_context(patch.object(dev.socket, "socket"))
        stack.enter_context(patch.object(dev.subprocess, "Popen", start))
        stack.enter_context(patch.object(dev.time, "sleep"))
        if fail_at == "timeout":
            stack.enter_context(patch.object(dev.time, "monotonic", side_effect=range(0, 10000, 60)))
        args = Namespace(compiler=Path("compiler/gcc.exe"), cmake="cmake", ninja="ninja",
                         server="server", programmer=Path("programmer/bin/cli.exe"), serial="0" * 24)
        try:
            dev.swd_download(args, build, directory)
        except (ValueError, RuntimeError):
            assert fail_at is not None
        else:
            assert fail_at is None
        assert process.stopped and steps[-1] == "release-probe"
        if fail_at in ("internal-backup", "backup", "write", "read", "readback"):
            assert "record" not in steps and "start-installer" not in steps
        elif fail_at == "record":
            assert steps.count("record") == 1 and "start-installer" not in steps
        elif fail_at == "start-installer":
            assert steps.count("start-installer") == 1 and "health-1" not in steps
        else:
            assert steps.index("record") < steps.index("start-installer") < steps.index("health-1")
        print(f"PASS：编排/释放进程，注入点={fail_at or '正常安装'}")


class MemorySocket:
    """只在内存收发真实18字节帧，既支持半包也支持粘包，不创建系统socket。"""

    def __init__(self, reply):
        self.reply, self.requests = reply, []
        self.pending, self.responses, self.sequence = bytearray(), bytearray(), 1

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass

    def settimeout(self, timeout):
        pass

    def sendall(self, wire):
        self.pending.extend(wire)
        while len(self.pending) >= 18:
            header = struct.unpack(">IHHHHIH", self.pending[:18])
            if len(self.pending) < header[0]:
                break
            body = json.loads(self.pending[18:header[0]])
            del self.pending[:header[0]]
            self.requests.append((header, body))
            command, response = self.reply(header[4], body)
            payload = json.dumps(response, separators=(",", ":")).encode()
            self.responses.extend(net.make_frame(command, payload, self.sequence))
            self.sequence += 1

    def recv(self, length):
        data = bytes(self.responses[:length])
        del self.responses[:length]
        return data


def check_network_actions():
    """验证新默认动作、显式旧动作、3000字节整帧和仅查询可选回退。"""
    for legacy in (False, True):
        sock = MemorySocket(lambda command, body: (0x0902, body))
        client = net.Client(sock, [], legacy_ota_actions=legacy)
        for action in net.MCU_ACTIONS:
            client.call({"action": action})
        actions = [body["action"] for _, body in sock.requests]
        assert actions == [action[4:] if legacy else action for action in net.MCU_ACTIONS]
        # 覆盖u32最大十进制offset，比实际2MiB MCU下载区的最长offset更保守。
        client.call({"action": "mcu_write", "packageId": "FFFFFFFF", "offset": 0xFFFFFFFF,
                     "data": base64.b64encode(bytes(3000)).decode()})
        assert sock.requests[-1][0][0] <= 4096
        print(f"PASS：{'旧版显式迁移' if legacy else '新版默认'}动作与3000字节块整帧={sock.requests[-1][0][0]}字节")

    # notSupported/传输失败均不允许自动将写入或提交换成旧动作再发。
    for action in ("mcu_begin", "mcu_write", "mcu_finish", "mcu_cancel"):
        sock = MemorySocket(lambda command, body: (6, {"status": "failed", "errorCode": "notSupported"}))
        net.Client(sock, []).call({"action": action}, expect=6, error="notSupported")
        assert [body["action"] for _, body in sock.requests] == [action]
        with patch.object(sock, "recv", side_effect=TimeoutError("注入接收超时")):
            try:
                net.Client(sock, []).call({"action": action})
            except TimeoutError:
                pass
            else:
                raise AssertionError("超时必须交给操作者，不能自动重试或降级")
        assert [body["action"] for _, body in sock.requests] == [action, action]

    for allow_legacy, error_code in ((False, "notSupported"), (True, "notSupported"), (True, "invalidState")):
        def reply(command, body):
            if body["action"] == "query":
                return 0x0903, {"action": "query", "confirmed": True}
            return 6, {"status": "failed", "errorCode": error_code}
        sock = MemorySocket(reply)
        command, _ = net.Client(sock, []).query_after_upgrade("FFFFFFFF", allow_legacy=allow_legacy)
        fallback = allow_legacy and error_code == "notSupported"
        assert len(sock.requests) == (2 if fallback else 1)
        assert command == (0x0903 if fallback else 6)
    print("PASS：写入/提交不自动降级；仅显式旧版迁移的notSupported查询允许只读回退")


def check_network_migration(legacy=False):
    """运行真实CLI编排；旧App迁移经历旧上传、只读查询旧App、新App确认三个阶段。"""
    with tempfile.TemporaryDirectory(prefix="ota-network-test-") as temporary, ExitStack() as stack:
        directory = Path(temporary)
        package = bytes(sample()[2])
        package_path, report_path = directory / "test.ota", directory / "report.json"
        package_path.write_bytes(package)
        sockets = []

        def connect(*args, **kwargs):
            connection_index = len(sockets)
            old = legacy and connection_index < 3
            def reply(command, body):
                if command == 2:
                    return 1, {}
                action = body["action"]
                if action.startswith("mcu_") == old:
                    return 6, {"status": "failed", "errorCode": "notSupported"}
                name = action if old else action[4:]
                response = {"action": action, "status": "completed"}
                if name == "query":
                    installed = connection_index >= (3 if legacy else 2)
                    response.update(confirmed=installed or connection_index == 0,
                                    state="confirmed" if installed or connection_index == 0 else "ready",
                                    firmwareVersion="0.1.9" if installed else "0.1.8")
                    return (0x0903 if installed else 0x0902), response
                if name == "begin":
                    assert body["packageSize"] == len(package)
                    response.update(nextOffset=0, blockSize=3000)
                elif name == "write":
                    offset = body["offset"]
                    raw = base64.b64decode(body["data"], validate=True)
                    assert raw == package[offset:offset + len(raw)]
                    response.update(offset=offset, nextOffset=offset + len(raw))
                elif name == "finish":
                    response.update(status="accepted", state="ready")
                elif name == "cancel":
                    response.update(status="failed", errorCode="invalidState")
                    return 6, response
                return 0x0902, response
            sock = MemorySocket(reply)
            sockets.append(sock)
            return sock

        argv = ["pc_ota_network_test.py", "--package", str(package_path), "--upgrade", "--report", str(report_path)]
        if legacy:
            argv.append("--legacy-ota-actions")
        stack.enter_context(patch.object(sys, "argv", argv))
        stack.enter_context(patch.object(net.socket, "create_connection", connect))
        stack.enter_context(patch.object(net.time, "sleep"))
        net.main()
        report = json.loads(report_path.read_text(encoding="utf-8"))
        assert "error" not in report and report["after"]["firmwareVersion"] == "0.1.9"
        assert report["after"]["action"] == "mcu_query"
        prefix = "" if legacy else "mcu_"
        assert [body["action"] for _, body in sockets[1].requests] == [
            prefix + name for name in ("begin", "write", "write", "finish", "finish", "cancel")]
        if legacy:
            assert [body["action"] for _, body in sockets[2].requests] == ["mcu_query", "query"]
        assert all(body.get("action") in (None, "mcu_query") for _, body in sockets[-1].requests)
        print(f"PASS：实际网络升级编排离线运行，legacy={legacy}，提交后切换新查询并确认目标版本")


def check_network_cli_flag():
    """日常入口仅把显式旧版参数传给网络工具，默认仍采用新动作。"""
    for legacy in (False, True):
        with tempfile.TemporaryDirectory(prefix="ota-cli-test-") as temporary, ExitStack() as stack:
            directory = Path(temporary)
            argv = ["ota_dev.py", "network", "--version", "0.1.9", "--cmake", "cmake",
                    "--ninja", "ninja", "--compiler", "gcc", "--confirm-idle"]
            if legacy:
                argv.append("--legacy-ota-actions")
            stack.enter_context(patch.object(sys, "argv", argv))
            stack.enter_context(patch.object(dev, "ROOT", directory))
            stack.enter_context(patch.object(dev, "build_package", return_value=directory))
            run_mock = stack.enter_context(patch.object(dev, "run"))
            stack.enter_context(patch.dict(dev.os.environ))
            dev.main()
            assert ("--legacy-ota-actions" in run_mock.call_args.args) == legacy
    print("PASS：日常network入口显式传递旧版迁移参数")


def check_power_simulation_build():
    """模拟选项传到双槽构建，输出目录隔离；此检查不执行构建或访问设备。"""
    for enabled in (False, True):
        args = Namespace(cmake="cmake", ninja="ninja", compiler="gcc", version="0.1.18",
                         configuration="Release", power_simulation=enabled)
        with patch.object(dev, "run") as calls:
            build = dev.build_package(args)
        assert ("--power-simulation" in calls.call_args_list[0].args) == enabled
        prefix = "ota-app-power-sim" if enabled else "ota-app"
        assert build.name == f"{prefix}-0.1.18-release-a1"
        assert build in calls.call_args_list[1].args
    print("PASS：电源模拟仅显式启用，复用正式OTA构建且不混用产物")


if __name__ == "__main__":
    print("以下为离线模拟，不连接网络、ST-Link或开发板。")
    check_network_actions()
    check_network_migration()
    check_network_migration(legacy=True)
    check_network_cli_flag()
    check_power_simulation_build()
    check_validation()
    for failure in (None, "internal-backup", "backup", "write", "read", "readback", "record", "start-installer", "timeout"):
        check_workflow(failure)
