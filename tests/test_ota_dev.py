#!/usr/bin/env python3
"""日常下载入口的离线回归：版本/槽保护、步骤顺序、失败不提交及释放自有进程。

不访问网络、ST-Link或开发板；运行：python -B -X utf8 tests/test_ota_dev.py。
"""
from argparse import Namespace
import base64
from contextlib import ExitStack, redirect_stderr
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import ota_dev as dev
import pc_ota_network_test as net
from test_ota_client import sample as full_package
sys.path.insert(0, str(dev.BOOT / "tools"))
sys.path.insert(0, str(dev.BOOT / "tests"))
import board_ota
import first_flash
import prepare_board_ota


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


def journal(record):
    """按现行256字节页格式制作已提交快照，不访问Flash。"""
    page = bytearray(256)
    page[:8] = b"BST1\x01\x00\x00\x01"
    page[8:60], page[64:248] = record[:52], record[52:]
    struct.pack_into("<2I", page, 248, zlib.crc32(page[:248]), 0xC001CAFE)
    nor = bytearray(b"\xff" * 0x410000)
    nor[0x4000:0x4100] = page
    return nor


def check_workflow(fail_at=None, board_name="apollo-v2", active=0, explicit_serial=False):
    """替换硬件边界；主编排本身真实运行，确认任何失败都会释放自有服务器。"""
    with tempfile.TemporaryDirectory(prefix="ota-dev-test-") as temporary, ExitStack() as stack:
        directory = Path(temporary)
        app, target = dev.TARGETS[board_name]
        build = directory / "ota-app-0.1.2-debug-a1"
        build.mkdir()
        record, internal, _ = sample(active)
        version_offset = 52 + active * 92 + 16
        struct.pack_into("<H", record, version_offset + 8, 1)
        record[version_offset + 60:version_offset + 76] = target.encode().ljust(16, b"\0")
        internal[:32] = (target.encode() + b"\0").ljust(32, b"\0")
        if fail_at == "board":
            internal[:32] = b"wrong-board".ljust(32, b"\0")
        package = full_package(target)
        struct.pack_into("<2I", record, 36, len(package), zlib.crc32(package[:36]))
        normal = bytearray(record)
        struct.pack_into("<I", normal, 4, 0)
        if fail_at == "pending":
            struct.pack_into("<I", normal, 20, 1)
        new_image = package[120:]
        moved = bytearray(new_image)
        struct.pack_into("<I", moved, 4, dev.SLOTS[1] + 9)
        (build / f"{app}.ota").write_bytes(package)
        (build / f"{app}.bin").write_bytes(new_image)
        (build / "a2_from_a1.bin").write_bytes(moved)
        boot_root = directory / "bootloader"
        agent_build = boot_root / "build" / f"daily-swd-{board_name}"
        agent_build.mkdir(parents=True)
        steps = []
        scripts = {}
        process = SimpleNamespace(started=False, stopped=False)
        process.poll = lambda: 0 if process.stopped else None
        process.terminate = lambda: setattr(process, "stopped", True)
        process.kill = process.terminate
        process.wait = lambda timeout: 0

        def start(*args, **kwargs):
            process.started = True
            assert args[0][args[0].index("-i") + 1] == "0" * 24
            kwargs["stdout"].write("Waiting for debugger connection\n")
            kwargs["stdout"].flush()
            return process

        def enumerate_probes(command, **kwargs):
            # 自动枚举必须提供Programmer位置；失败输出含序列号也不能继续连接。
            steps.append("enumerate-probes")
            assert not explicit_serial
            assert command == ["server", "--debuggers", "-cp", str(Path("programmer/bin"))]
            assert kwargs["check"] is False and kwargs["capture_output"] and kwargs["timeout"] == 15
            output = "ST-LINK:" + "0" * 24 + "\n"
            if fail_at == "probe-missing":
                output = "No ST-Link connected\n"
            elif fail_at == "probe-multiple":
                output += "ST-LINK:" + "F" * 24 + "\n"
            return subprocess.CompletedProcess(command, 0x8004FFFD if fail_at == "probe-error" else 0,
                                               output, "ERROR: use -cp <path>\n" if fail_at == "probe-error" else "")

        def run_script(output, name, commands, **kwargs):
            steps.append(name)
            scripts[name] = commands
            assert "load" not in commands, "编排不可通过GDB load覆盖App/B"
            if name == fail_at:
                raise RuntimeError("注入GDB失败")
            if name == "internal-backup":
                (directory / "original-internal.bin").write_bytes(internal)
                (directory / "debug-freeze.bin").write_bytes(struct.pack("<I", 0x1234))
                (directory / "debug-demcr.bin").write_bytes(struct.pack("<I", 0x1000000))
            if name.startswith("health-") and fail_at != "timeout":
                slot = 1 - active
                (directory / "vtor.bin").write_bytes(struct.pack("<I", dev.SLOTS[slot]))
                (directory / "health.bin").write_bytes(struct.pack("<8I", 0x4F544131, dev.SLOTS[slot], 0, 11, 15, 30001, 0, 0))
                installed = bytearray(internal)
                start = dev.SLOTS[slot] - 0x08000000
                installed[start:start + len(new_image)] = new_image if slot == 0 else moved
                (directory / "installed-internal.bin").write_bytes(installed)

        def action(argv):
            steps.append(argv[0])
            assert "--reuse-agent" in argv, "备份/写入/回读/提交不能反复复位或重新load"
            if argv[0] == fail_at:
                raise RuntimeError("注入失败")
            if argv[0] == "write":
                assert argv[argv.index("--address") + 1] == "0x10000"
                assert steps.index("backup") < steps.index("write")
            if argv[0] == "backup":
                (directory / "original-w25-0-410000.bin").write_bytes(journal(normal))
            if argv[0] == "read":
                data = (directory / "upgrade-package-4k.bin").read_bytes()
                (directory / "stage-readback.bin").write_bytes(data if fail_at != "readback" else b"bad")
            if argv[0] == "record":
                assert steps.index("read") < steps.index("record")

        def run(*command):
            if "binary" in command:
                Path(command[-1]).write_bytes(bytes(16))
            if any(str(part).endswith("prepare_board_ota.py") for part in command):
                steps.append("prepare")
                assert "--journal" in command
                assert command[command.index("--app-name") + 1] == app
                (directory / "upgrade-record.bin").write_bytes(record)
                (directory / "upgrade-package-4k.bin").write_bytes(package + b"\xff" * (-len(package) % 4096))

        def build_after_backup(args):
            steps.append("build")
            assert args.version == "0.1.2" and args.board == board_name
            if fail_at == "build":
                raise RuntimeError("注入编译失败")
            return build

        board = SimpleNamespace(run_script=run_script, main=action)
        stack.enter_context(patch.dict(sys.modules, {"board_ota": board}))
        stack.enter_context(patch.object(first_flash, "check_elf"))
        stack.enter_context(patch.object(dev, "BOOT", boot_root))
        stack.enter_context(patch.object(dev, "build_package", build_after_backup))
        stack.enter_context(patch.object(dev, "run", run))
        stack.enter_context(patch.object(dev.socket, "socket"))
        stack.enter_context(patch.object(dev.subprocess, "run", enumerate_probes))
        stack.enter_context(patch.object(dev.subprocess, "Popen", start))
        stack.enter_context(patch.object(dev.time, "sleep"))
        if fail_at == "timeout":
            stack.enter_context(patch.object(dev.time, "monotonic", side_effect=range(0, 10000, 60)))
        args = Namespace(compiler=Path("compiler/gcc.exe"), cmake="cmake", ninja="ninja",
                         server="server", programmer=Path("programmer/bin/cli.exe"), serial="0" * 24 if explicit_serial else "",
                         board=board_name, version="auto")
        failure_message = ""
        try:
            dev.swd_download(args, None, directory)
        except (ValueError, RuntimeError) as error:
            assert fail_at is not None
            failure_message = str(error)
        else:
            assert fail_at is None
        if fail_at in ("probe-error", "probe-missing", "probe-multiple"):
            assert not process.started and steps == ["enumerate-probes"]
            log = (directory / "probes.log").read_text(encoding="utf-8")
            if fail_at == "probe-error":
                assert "0x8004FFFD" in log and "ERROR: use -cp <path>" in log
                assert "0x8004FFFD" in failure_message and "ERROR: use -cp <path>" in failure_message
            else:
                expected = "No ST-Link" if fail_at == "probe-missing" else "F" * 24
                assert expected in log
            print(f"PASS：{board_name}枚举拒绝并保留日志，不连接/复位MCU，注入点={fail_at}")
            return
        assert args.serial == "0" * 24
        assert (directory / "probes.log").exists() != explicit_serial
        assert process.stopped and steps[-1] == "release-probe"
        if fail_at in ("internal-backup", "board", "init-agent", "backup", "pending", "build", "write", "read", "readback"):
            assert "record" not in steps and "start-installer" not in steps
        elif fail_at == "record":
            assert steps.count("record") == 1 and "start-installer" not in steps
        elif fail_at == "start-installer":
            assert steps.count("start-installer") == 1 and "health-1" not in steps
            assert "monitor reset" not in scripts["release-probe"], "启动结果不确定时不能自动再次复位"
        else:
            assert steps.index("record") < steps.index("start-installer") < steps.index("health-1")
        if "init-agent" in scripts:
            assert scripts["init-agent"].count("monitor reset") == 1
            assert any("restore " in line and "binary 0x24000000" in line for line in scripts["init-agent"])
            assert any("0x08000004" in line for line in scripts["init-agent"])
        if fail_at is None:
            assert "monitor reset" not in scripts["release-probe"]
            assert steps.index("backup") < steps.index("build") < steps.index("write")
        if fail_at != "internal-backup":
            assert any("debug-freeze.bin" in line for line in scripts["release-probe"])
            assert any("debug-demcr.bin" in line for line in scripts["release-probe"])
        print(f"PASS：{board_name}/A{active + 1}编排/释放进程，注入点={fail_at or '正常安装'}")


def check_one_click():
    """检查无交互入口、自动版本、唯一探针选择与助手复用，不启动任何真实服务。"""
    assert dev.next_version((0, 1, 36)) == "0.1.37"
    assert dev.next_version((1, 9, 65534)) == "1.9.65535"
    rejected(lambda: dev.next_version((1, 9, 65535)))
    serial = "54FF6D067182505239431087"
    assert dev.select_serial("ST-LINK SN : " + serial) == serial
    assert dev.select_serial(serial + "\n" + serial.lower()) == serial
    rejected(lambda: dev.select_serial("No ST-Link connected"))
    rejected(lambda: dev.select_serial(serial + "\n" + "F" * 24))
    for root, simulation in ((dev.ROOT, True), (dev.ROOT.parent / "b1", False)):
        config = json.loads((root / ".vscode/tasks.json").read_text(encoding="utf-8"))
        defaults = [task for task in config["tasks"] if isinstance(task.get("group"), dict)
                    and task["group"].get("isDefault")]
        assert len(defaults) == 1
        args = defaults[0]["args"]
        assert "ota-swd" in args and args[args.index("-ConfirmIdle") + 1] == "YES"
        assert not any("${input:" in value for value in args)
        assert ("-PowerSimulation" in args) == simulation
        assert ("-MatsFailTest" in args) == simulation
    commands = board_ota.begin_agent(reuse=True)
    assert "load" not in commands and not any("reset" in line for line in commands)
    assert "monitor reset" in board_ota.begin_agent(), "旧维护/首烧入口行为仍须保留"
    normal = sample()[0]
    struct.pack_into("<I", normal, 4, 0)
    assert prepare_board_ota.latest_record(journal(normal)) == normal
    broken = journal(normal)
    broken[0x4008] ^= 1
    rejected(lambda: prepare_board_ota.latest_record(broken))
    print("PASS：双板无交互快捷键、自动版本/探针、已提交日志及无重复复位")


def check_swd_cli_guards():
    """真实参数解析仅替换下载边界；拒绝项不得进入硬件编排或构建。"""
    for board_name in dev.TARGETS:
        with tempfile.TemporaryDirectory(prefix="ota-cli-guard-") as temporary, ExitStack() as stack:
            argv = ["ota_dev.py", "swd", "--board", board_name, "--app-root", temporary,
                    "--cmake", "cmake", "--ninja", "ninja", "--compiler", "gcc",
                    "--server", "server", "--programmer", "programmer"]
            download = stack.enter_context(patch.object(dev, "swd_download"))
            build = stack.enter_context(patch.object(dev, "build_package"))
            stack.enter_context(patch.dict(dev.os.environ))
            for extra in ([], ["--confirm-idle", "--serial", "bad"],
                          ["--confirm-idle", "--version", "65536.0.0"],
                          ["--confirm-idle", "--mats-fail-test"],
                          *([["--confirm-idle", "--power-simulation"]] if board_name == "b1" else [])):
                with patch.object(sys, "argv", argv + extra), redirect_stderr(io.StringIO()):
                    try:
                        dev.main()
                    except SystemExit as error:
                        assert error.code == 2
                    else:
                        raise AssertionError("危险或无效参数未拒绝")
                download.assert_not_called()
                build.assert_not_called()
            with patch.object(sys, "argv", argv + ["--confirm-idle"]):
                dev.main()
            args, build_path, directory = download.call_args.args
            assert args.board == board_name and args.version == "auto" and args.serial == ""
            assert args.app_root == Path(temporary).resolve() and build_path is None
            assert directory.is_dir() and directory.is_relative_to(Path(temporary))
            build.assert_not_called()
    print("PASS：双板参数入口、自动版本/探针缺省值及空闲/板型模拟门禁")


def check_gdb_boot_guard():
    """实际GDB只计算本机常量：验证指针类型/Thumb位，不连接目标或读取硬件。"""
    gdb = board_ota.GNU / "arm-none-eabi-gdb.exe"
    if not gdb.is_file():
        print("SKIP：未找到Arm GDB，复位入口表达式的实际解释器检查未执行")
        return
    checks = [line[3:] for line in dev.init_agent_commands(Path("agent.elf"), Path("agent.bin"))
              if line.startswith("if ") and "$pc" in line]
    assert len(checks) == 1
    # 只替换寄存器值与向量内存读取，保留生产表达式的类型转换和比较逻辑。
    expression = checks[0].replace("$pc", "$probe_pc").replace(
        "*(unsigned int*)0x08000004", "$probe_vector")
    commands = ["set architecture armv7e-m", "set language c",
                "set $probe_vector = (unsigned int)0x08000299"]
    for pointer_type in ("void *", "void (*)()"):
        for address, mismatch in ((0x08000298, 0), (0x08000299, 0), (0x08107A9E, 1),
                                  (0x24000000, 1), (0, 1)):
            commands += [f"set $probe_pc = ({pointer_type}){address}",
                         f"if ({expression}) != {mismatch}", "quit 3", "end"]
    commands += ["echo PASS: boot guard expressions\\n", "quit 0"]
    with tempfile.TemporaryDirectory(prefix="ota-gdb-guard-") as temporary:
        script = Path(temporary) / "guard.gdb"
        script.write_text("\n".join(commands) + "\n", encoding="utf-8")
        result = subprocess.run([str(gdb), "-nx", "-q", "--batch", "-x", str(script)],
                                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=15)
        output = result.stdout + result.stderr
        assert result.returncode == 0 and "PASS: boot guard expressions" in output, output
    print("PASS：实际GDB的10种复位入口表达式，正确入口接受，App/RAM/零地址拒绝")


def check_powershell_entry():
    """执行实际PS入口分支，但用内存函数接收Python参数，不运行下载器或编译器。"""
    shell = shutil.which("powershell.exe")
    if shell is None:
        print("SKIP：未安装Windows PowerShell，双板PS实参检查未执行")
        return
    script = r"""
$ErrorActionPreference = 'Stop'
$repo = '__ROOT__'
$Task = 'ota-swd'; $Version = ''; $Serial = '__SERIAL__'; $ConfirmIdle = 'YES'
$Configuration = 'Debug'; $PowerSimulation = $false; $MatsFailTest = $false
$LogSource = ''; $python = 'Capture-Python'; $cmake = 'offline-cmake'; $ninja = 'offline-ninja'
function Find-Python { return 'Capture-Python' }
function Find-BundledExe { return 'offline-tool' }
function Capture-Python { ConvertTo-Json -InputObject @($args) -Compress; $global:LASTEXITCODE = 0 }
$tokens = $null; $errors = $null
$tree = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $repo 'tools/dev.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'PowerShell parse failed' }
$branches = @($tree.EndBlock.Statements | Where-Object {
    $_ -is [System.Management.Automation.Language.IfStatementAst] -and $_.Extent.Text -match '^if \(\$Task .*ota-swd'
})
if ($branches.Count -ne 1) { throw 'Expected exactly one SWD entry' }
Invoke-Expression $branches[0].Extent.Text
"""
    for root, board_name in ((dev.ROOT, "apollo-v2"), (dev.ROOT.parent / "b1", "b1")):
        for serial in ("", "54FF6D067182505239431087"):
            command = script.replace("__ROOT__", str(root).replace("'", "''")).replace("__SERIAL__", serial)
            result = subprocess.run([shell, "-NoProfile", "-NonInteractive", "-Command", command],
                                    check=True, capture_output=True, text=True, encoding="utf-8", timeout=20)
            arguments = json.loads(result.stdout)
            assert arguments[arguments.index("--version") + 1] == "auto"
            assert "swd" in arguments and "--confirm-idle" in arguments
            assert ("--serial" in arguments) == bool(serial)
            if serial:
                assert arguments[arguments.index("--serial") + 1] == serial
            if board_name == "b1":
                assert arguments[arguments.index("--board") + 1] == "b1"
                assert "--power-simulation" not in arguments and "--mats-fail-test" not in arguments
    print("PASS：真实PowerShell双板入口的自动版本、空/指定序列号与B1固定板型参数")


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
    for enabled, mats_fail in ((False, False), (True, False), (True, True)):
        args = Namespace(cmake="cmake", ninja="ninja", compiler="gcc", version="0.1.18",
                         configuration="Release", power_simulation=enabled, mats_fail_test=mats_fail)
        with patch.object(dev, "run") as calls:
            build = dev.build_package(args)
        assert ("--power-simulation" in calls.call_args_list[0].args) == enabled
        assert ("--mats-fail-test" in calls.call_args_list[0].args) == mats_fail
        prefix = "ota-app-power-sim" if enabled else "ota-app"
        if mats_fail:
            prefix += "-mats-fail"
        assert build.name == f"{prefix}-0.1.18-release-a1"
        assert build in calls.call_args_list[1].args
    print("PASS：电源模拟及MATS失败联调仅显式启用，复用正式OTA且不混用产物")


if __name__ == "__main__":
    print("以下为离线模拟，不连接网络、ST-Link或开发板。")
    check_network_actions()
    check_network_migration()
    check_network_migration(legacy=True)
    check_network_cli_flag()
    check_power_simulation_build()
    check_validation()
    check_one_click()
    check_swd_cli_guards()
    check_gdb_boot_guard()
    check_powershell_entry()
    for board_name in dev.TARGETS:
        check_workflow(board_name=board_name, explicit_serial=True)
        for active in (0, 1):
            check_workflow(board_name=board_name, active=active)
        for failure in ("probe-error", "probe-missing", "probe-multiple", "internal-backup", "board", "init-agent", "backup", "pending", "build", "write",
                        "read", "readback", "record", "start-installer", "timeout"):
            check_workflow(failure, board_name=board_name)
