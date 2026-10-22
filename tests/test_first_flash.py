#!/usr/bin/env python3
"""首烧入口离线回归：双板型、误包/非空保护、读回失败、提交顺序及调试器释放。

只替换设备边界，执行真实Python编排；不连接ST-Link、网络或串口。
运行：python -B -X utf8 tests/test_first_flash.py。
"""
from argparse import Namespace
from contextlib import ExitStack, redirect_stderr
import io
import json
from pathlib import Path
import struct
import shutil
import subprocess
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
            # 清单中的新开关保持向后兼容；非布尔、B1或非模拟假FAIL均拒绝。
            manifest["power_simulation"] = False
            for mats in ("false", 1, True):
                manifest["mats_fail_test"] = mats
                save_manifest(directory, manifest)
                rejected(lambda: first.validate_bundle(directory, board))
            manifest["power_simulation"] = True
            if board == "apollo-v2":
                save_manifest(directory, manifest)
                first.validate_bundle(directory, board)
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


def check_deploy_entry():
    """执行真实CLI分流，替换构建/设备边界：只消费本次产物，失败不得烧旧包。"""
    tools = ["--compiler", "offline-gcc", "--cmake", "offline-cmake", "--ninja", "offline-ninja",
             "--app-root", "offline-app", "--server", "offline-server", "--programmer", "offline-cli"]
    consent = ["--serial", "0" * 24, "--confirm-idle", "--confirm-blank"]
    for board in first.TARGETS:
        argv = ["first_flash.py", "deploy", "--board", board, "--version", "0.1.37", *tools, *consent]
        if board == "apollo-v2":
            argv += ["--power-simulation", "--mats-fail-test"]
        built = Path("exact-new-bundle")
        events = []

        def build(args):
            assert args.board == board and args.version == "0.1.37"
            assert args.power_simulation == (board == "apollo-v2")
            assert args.mats_fail_test == (board == "apollo-v2")
            events.append("build")
            return built

        def flash(args):
            assert args.bundle == built and events == ["build"]
            assert args.board == board and args.confirm_idle and args.confirm_blank
            events.append("flash")

        with patch.object(sys, "argv", argv), patch.object(first, "build_bundle", build), \
                patch.object(first, "flash_bundle", flash):
            first.main()
        assert events == ["build", "flash"]
        with patch.object(sys, "argv", argv), patch.object(first, "build_bundle", side_effect=RuntimeError("构建失败")), \
                patch.object(first, "flash_bundle") as flash_mock:
            rejected(first.main)
            flash_mock.assert_not_called()

        # 不确认/无序列号/无工具时，在耗时构建前就停止，更不能连接探针。
        for missing in ("--confirm-idle", "--confirm-blank", "--serial", "--server"):
            bad = argv.copy()
            index = bad.index(missing)
            del bad[index:index + (1 if missing.startswith("--confirm") else 2)]
            with patch.object(sys, "argv", bad), patch.object(first, "build_bundle") as build_mock, \
                    patch.object(first, "flash_bundle") as flash_mock, redirect_stderr(io.StringIO()):
                try:
                    first.main()
                except SystemExit as error:
                    assert error.code == 2
                else:
                    raise AssertionError("首烧未确认/工具缺失却未拒绝")
                build_mock.assert_not_called()
                flash_mock.assert_not_called()

    for extra in (["--power-simulation"], ["--mats-fail-test"], ["--rk-uart", "2"]):
        argv = ["first_flash.py", "deploy", "--board", "b1", "--version", "0.1.37", *tools, *consent, *extra]
        with patch.object(sys, "argv", argv), patch.object(first, "build_bundle") as build_mock, \
                patch.object(first, "flash_bundle") as flash_mock, redirect_stderr(io.StringIO()):
            try:
                first.main()
            except SystemExit as error:
                assert error.code == 2
            else:
                raise AssertionError("B1非法配置未拒绝")
            build_mock.assert_not_called()
            flash_mock.assert_not_called()
    print("PASS：双板deploy只烧本次构建、失败不烧、先确认再构建、B1配置隔离")


def check_powershell_and_tasks():
    """在PowerShell中捕获参数，不运行Python或烧录器；同时检查两个VS Code任务。"""
    shell = shutil.which("powershell.exe")
    if not shell:
        print("SKIP：未安装Windows PowerShell，真实PS参数传递检查未执行")
        return
    script = r"""
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
function Capture-Python { ConvertTo-Json -InputObject @($args) -Compress; $global:LASTEXITCODE = 0 }
& '__ENTRY__' -Action deploy -Board __BOARD__ -AppRoot 'offline-app' -Version 0.1.37 `
    -Serial 000000000000000000000000 -ConfirmIdle YES -ConfirmBlank __CONFIRM__ `
    -Python Capture-Python -Compiler offline-gcc -CMake offline-cmake -Ninja offline-ninja `
    -Server offline-server -Programmer offline-cli __FLAGS__
"""
    for board, root in (("apollo-v2", BOOT.parent / "test"), ("b1", BOOT.parent / "b1")):
        tasks = json.loads((root / ".vscode/tasks.json").read_text(encoding="utf-8-sig"))
        entry = [task for task in tasks["tasks"] if "Boot+App" in task["label"]]
        assert len(entry) == 1 and entry[0]["group"] == "build"
        args = entry[0]["args"]
        assert args[args.index("-Action") + 1] == "deploy"
        assert args[args.index("-ConfirmIdle") + 1] == "${input:firstFlashConfirm}"
        assert args[args.index("-ConfirmBlank") + 1] == "${input:firstFlashConfirm}"
        assert args[args.index("-Serial") + 1] == "${input:firstFlashSerial}"
        assert ("-PowerSimulation" in args) == (board == "apollo-v2")
        assert ("-MatsFailTest" in args) == (board == "apollo-v2")
        confirms = [item for item in tasks["inputs"] if item["id"] == "firstFlashConfirm"]
        assert len(confirms) == 1 and confirms[0]["default"] == "NO"
        defaults = [task for task in tasks["tasks"] if isinstance(task.get("group"), dict)
                    and task["group"].get("isDefault")]
        assert len(defaults) == 1 and "ota-swd" in defaults[0]["args"]
        for confirm in ("NO", "YES"):
            command = (script.replace("__ENTRY__", str(BOOT / "tools/flash.ps1").replace("'", "''"))
                       .replace("__BOARD__", board).replace("__CONFIRM__", confirm)
                       .replace("__FLAGS__", "-PowerSimulation -MatsFailTest" if board == "apollo-v2" else ""))
            result = subprocess.run([shell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", command],
                                    capture_output=True, text=True, encoding="utf-8", timeout=20)
            if confirm == "NO":
                assert result.returncode != 0 and not result.stdout.strip()
                continue
            assert result.returncode == 0, result.stderr
            passed = json.loads(result.stdout)
            assert passed[passed.index("--board") + 1] == board and "deploy" in passed
            assert passed[passed.index("--version") + 1] == "0.1.37"
            assert passed[passed.index("--serial") + 1] == "0" * 24
            assert "--confirm-idle" in passed and "--confirm-blank" in passed
            assert "--bundle" not in passed and passed.count("--compiler") == 1
            assert ("--power-simulation" in passed) == (board == "apollo-v2")
            assert ("--mats-fail-test" in passed) == (board == "apollo-v2")
    print("PASS：双板真实PowerShell参数、取消保护、VS Code首烧独立且不改变日常默认任务")


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
    check_deploy_entry()
    check_powershell_and_tasks()
    for target in first.TARGETS:
        for error in (None, "internal-backup", "backup", "not-blank", "install-internal", "bad-internal",
                      "write", "read", "fru-change", "record", "start-app", "extra-reset", "timeout"):
            check_flow(target, error)
