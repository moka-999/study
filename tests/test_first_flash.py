#!/usr/bin/env python3
"""首烧入口离线回归：双板型、误包/非空保护、读回失败、提交顺序及调试器释放。

只替换设备边界，执行真实Python编排；不连接ST-Link、网络或串口。
运行：python -B -X utf8 tests/test_first_flash.py。
"""
from argparse import Namespace
from contextlib import ExitStack
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
import zlib

BOOT = Path(__file__).resolve().parents[2] / "bootloader"
sys.path.insert(0, str(BOOT / "tools"))
import first_flash as first


def rejected(call):
    try:
        call()
    except (ValueError, RuntimeError):
        return
    raise AssertionError("非法输入未拒绝")


def make_bundle(directory, board="apollo-v2"):
    """构造合法且极小的双板型文件样本；ELF为占位，设备编排测试会替换ELF工具。"""
    target = first.TARGETS[board][1]
    image = struct.pack("<4I", 0x20020000, first.A1 + 9, 0, 0)
    version = bytearray(76)
    struct.pack_into("<5H", version, 0, 1, 76, 0, 1, 23)
    struct.pack_into("<2I", version, 12, len(image), zlib.crc32(image))
    version[20:44] = b"2026-09-30T00:00:00Z".ljust(24, b"\0")
    version[44:60] = b"offline-test".ljust(16, b"\0")
    version[60:76] = target.encode().ljust(16, b"\0")
    a2 = bytearray(image)
    struct.pack_into("<I", a2, 4, 0x08100009)
    package = first.pack.build_package(image, bytes(version), [4], bytes(a2))
    for name in first.FILES:
        (directory / name).write_bytes(b"placeholder")
    (directory / "boot.bin").write_bytes(struct.pack("<2I", 0x20020000, 0x08000009) + target.encode() + b"\0")
    (directory / "app.bin").write_bytes(image)
    (directory / "firmware.ota").write_bytes(package)
    (directory / "initial-record.bin").write_bytes(first.prepare.initial_record(package, image))
    (directory / "stable-4k.bin").write_bytes(package + b"\xff" * (-len(package) % 4096))
    manifest = {"format": 1, "board": board, "version": "0.1.23", "power_simulation": False,
                "rk_uart": 4 if board == "b1" else 1}
    save_manifest(directory, manifest)
    return manifest


def save_manifest(directory, manifest):
    manifest["sha256"] = {name: first.sha(directory / name) for name in first.FILES}
    (directory / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")


def check_validation():
    # 默认日常维护仍先选ELF再连接/复位；新增首烧路径只复用不复位的RAM加载部分。
    maintenance = first.board.begin_agent()
    assert maintenance[3].startswith('file "')
    assert maintenance[4:8] == ["target extended-remote localhost:61234", "monitor reset", "monitor halt", "load"]
    assert "monitor reset" not in first.board.load_agent()
    for board in first.TARGETS:
        with tempfile.TemporaryDirectory(prefix="first-flash-") as temp:
            directory = Path(temp)
            manifest = make_bundle(directory, board)
            first.validate_bundle(directory, board)
            other = "b1" if board == "apollo-v2" else "apollo-v2"
            rejected(lambda: first.validate_bundle(directory, other))
            for name in first.FILES:
                data = (directory / name).read_bytes()
                (directory / name).write_bytes(data + b"x")
                rejected(lambda: first.validate_bundle(directory, board))
                (directory / name).write_bytes(data)
            # 即使同步了清单散列，也不能将损坏CRC、跨区修正、其他板型包当成有效包。
            package = (directory / "firmware.ota").read_bytes()
            image = (directory / "app.bin").read_bytes()
            for offset in (0, 12, 16, 20, 24, 28, 36, 40, 100, 116, len(package) - 1):
                broken = bytearray(package)
                broken[offset] ^= 1
                rejected(lambda: first.validate_package(bytes(broken), image, first.TARGETS[board][1]))
            initial = (directory / "initial-record.bin").read_bytes()
            (directory / "initial-record.bin").write_bytes(bytes(236))
            save_manifest(directory, manifest)
            rejected(lambda: first.validate_bundle(directory, board))
            (directory / "initial-record.bin").write_bytes(initial)
            save_manifest(directory, manifest)
            if board == "b1":
                manifest["power_simulation"] = True
                save_manifest(directory, manifest)
                rejected(lambda: first.validate_bundle(directory, board))
    internal = b"\xff" * 0x200000
    nor = bytearray(b"\xff" * first.NOR_BYTES)
    nor[0x2000:0x2004] = b"FRU!"
    nor[0x10000:0x10004] = b"old!"
    first.require_blank(internal, bytes(nor))
    for address in (0, 0x1000, 0x4000, 0x5FFF, first.STABLE, first.NOR_BYTES - 1):
        bad = bytearray(nor)
        bad[address] = 0
        rejected(lambda: first.require_blank(internal, bytes(bad)))
    rejected(lambda: first.require_blank(b"x" + internal[1:], bytes(nor)))
    rejected(lambda: first.require_blank(internal[:-1], bytes(nor)))
    print("PASS：双板型、包CRC/修正/初始记录、文件散列及空板保护；FRU不克隆")


def check_flow(board_name, failure=None):
    """故障注入不创建真实socket或进程；核对提交总在所有回读之后。"""
    with tempfile.TemporaryDirectory(prefix="first-flash-") as temp, ExitStack() as stack:
        bundle = Path(temp)
        make_bundle(bundle, board_name)
        steps = []
        process = SimpleNamespace(stopped=False)
        process.poll = lambda: 0 if process.stopped else None
        process.terminate = lambda: setattr(process, "stopped", True)
        process.kill = process.terminate
        process.wait = lambda timeout: 0
        nor = bytearray(b"\xff" * first.NOR_BYTES)
        nor[0x2000:0x2004] = b"FRU!"
        image = bytearray(b"\xff" * 0x200000)
        for name, offset in (("boot.bin", 0), ("app.bin", 0x20000)):
            data = (bundle / name).read_bytes()
            image[offset:offset + len(data)] = data

        def launch(*args, **kwargs):
            kwargs["stdout"].write("Waiting for debugger connection\n")
            kwargs["stdout"].flush()
            return process

        def gdb(directory, name, commands, **kwargs):
            steps.append(name)
            if name == failure:
                raise RuntimeError("注入GDB失败")
            if name == "internal-backup":
                assert "load" not in commands
                (directory / "original-internal.bin").write_bytes(b"\xff" * 0x200000)
            elif name == "install-internal":
                assert steps.index("backup") < steps.index(name)
                (directory / "installed-internal.bin").write_bytes(image if failure != "bad-internal" else b"bad")
            elif name.startswith("health-"):
                assert "monitor reset" not in commands and "load" not in commands
                epoch = 2 if failure == "extra-reset" else 1
                health_time = 0 if failure == "timeout" else 31000
                diag = (0x4F544131, first.A1, 0, epoch, 15, health_time, 0, 0)
                (directory / "health.bin").write_bytes(struct.pack("<8I", *diag))
                (directory / "vtor.bin").write_bytes(struct.pack("<I", first.A1))
            elif name == "release-probe" and "start-app" in steps and failure != "start-app":
                assert "monitor reset" not in commands

        def action(argv):
            kind = argv[0]
            steps.append(kind)
            directory = Path(argv[argv.index("--directory") + 1])
            if kind == failure:
                raise RuntimeError("注入维护失败")
            if kind == "backup":
                data = bytearray(nor)
                if failure == "not-blank":
                    data[0x4000] = 0
                (directory / "original-w25-0-410000.bin").write_bytes(data)
            elif kind == "write":
                assert int(argv[argv.index("--address") + 1], 0) == first.STABLE
                assert steps.index("install-internal") < steps.index("write")
            elif kind == "read":
                data = bytearray(nor)
                stable = (bundle / "stable-4k.bin").read_bytes()
                data[first.STABLE:first.STABLE + len(stable)] = stable
                if failure == "fru-change":
                    data[0x2000] ^= 1
                (directory / "verify-nor.bin").write_bytes(data)
            elif kind == "record":
                assert steps.index("read") < steps.index("record")
                assert Path(argv[argv.index("--input") + 1]).name == "initial-record.bin"

        fake = SimpleNamespace(run_script=gdb, main=action, load_agent=lambda: ["load-ram-agent"])
        stack.enter_context(patch.object(first, "board", fake))
        stack.enter_context(patch.object(first, "check_elf"))
        stack.enter_context(patch.object(first.socket, "socket"))
        stack.enter_context(patch.object(first.subprocess, "Popen", launch))
        stack.enter_context(patch.object(first.time, "sleep"))
        stack.enter_context(patch.object(first.time, "monotonic", side_effect=range(0, 10000, 10)))
        args = Namespace(bundle=bundle, board=board_name, serial="0" * 24,
                         confirm_idle=True, confirm_blank=True, compiler=Path("gnu/gcc.exe"),
                         server="server", programmer=Path("programmer/cli.exe"))
        try:
            first.flash_bundle(args)
        except (ValueError, RuntimeError, TimeoutError):
            assert failure is not None
        else:
            assert failure is None
        assert process.stopped and steps[-1] == "release-probe"
        reports = list((bundle / "records").glob("*/report.json"))
        report = json.loads(reports[0].read_text(encoding="utf-8"))
        assert (report["result"] == "passed") == (failure is None)
        if failure in ("internal-backup", "backup", "not-blank"):
            assert "install-internal" not in steps
        if failure in ("internal-backup", "backup", "not-blank", "install-internal", "bad-internal", "write", "read", "fru-change"):
            assert "record" not in steps and "start-app" not in steps
        if failure is None:
            assert steps.index("record") < steps.index("start-app") < steps.index("health-1")
        print(f"PASS：{board_name} 首烧编排与释放，故障点={failure or '无'}")


if __name__ == "__main__":
    check_validation()
    for target in first.TARGETS:
        for error in (None, "internal-backup", "backup", "not-blank", "install-internal", "bad-internal",
                      "write", "read", "fru-change", "record", "start-app", "extra-reset", "timeout"):
            check_flow(target, error)
