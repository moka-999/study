#!/usr/bin/env python3
"""V2/B1空板首次部署：离线生成可转交的固定包，显式授权后才通过SWD烧写。

只接纳内部Flash全空、W25版本/启动日志/稳定区全空的板卡；不提供强制覆盖。
不改选项字节、不连接TCP/RK、不克隆FRU；任何失败均保留备份并停止。
"""
from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import time

import board_ota as board
import pack_ota as pack

ROOT = Path(__file__).resolve().parents[1]
sys.path.append(str(ROOT / "tests"))
import prepare_board_ota as prepare

TARGETS = {"apollo-v2": ("test", "apollo-v2-h743"), "b1": ("b1", "b1-h743")}
A1 = 0x08020000
STABLE = 0x210000
NOR_BYTES = 0x410000
FILES = ("boot.bin", "boot.elf", "app.bin", "app.elf", "firmware.ota",
         "boot_ram_agent.elf", "initial-record.bin", "stable-4k.bin",
         "first_flash.py", "board_ota.py", "pack_ota.py", "prepare_board_ota.py", "flash.ps1")


def run(*args: object) -> None:
    """子程序失败立即停止，禁止自动重试擦写。"""
    subprocess.run([str(value) for value in args], check=True)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_package(package: bytes, image: bytes, target: str) -> bytes:
    """复用正式打包器反向核对CRC/字段/修正表，返回匹配A1的初始RAM记录。"""
    if not 116 <= len(package) <= pack.PACKAGE_LIMIT:
        raise ValueError("升级包长度非法")
    count = struct.unpack_from("<I", package, 16)[0]
    if count == 0 or 116 + count * 4 + len(image) != len(package):
        raise ValueError("修正表/镜像边界不符")
    version = package[40:116]
    if version[60:76] != target.encode().ljust(16, b"\0"):
        raise ValueError("升级包板型不符；V2/B1禁止混刷")
    offsets = list(struct.unpack_from(f"<{count}I", package, 116))
    moved = bytearray(image)
    previous = 0
    for offset in offsets:
        if not previous + 4 <= offset <= len(image) - 4:
            raise ValueError("修正表重复、乱序或越界")
        value = struct.unpack_from("<I", image, offset)[0]
        if not A1 <= value <= A1 + len(image):
            raise ValueError("修正表指向RAM/外设或镜像外")
        struct.pack_into("<I", moved, offset, value + 0xE0000)
        previous = offset
    if pack.build_package(image, version, offsets, bytes(moved)) != package:
        raise ValueError("OTA包CRC/字段/镜像不一致")
    return prepare.initial_record(package, image)


def validate_bundle(directory: Path, expected_board: str) -> dict:
    """散列防止误拷贝/缺件，不是数字签名；只使用固定文件名和固定Flash地址。"""
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    if (manifest.get("format") != 1 or manifest.get("board") != expected_board
            or set(manifest.get("sha256", {})) != set(FILES)):
        raise ValueError("首烧包格式、板型或文件列表不符")
    if (type(manifest.get("power_simulation")) is not bool
            or manifest.get("rk_uart") not in ((2, 4) if expected_board == "b1" else (1,))):
        raise ValueError("模拟标志/RK串口配置无效")
    # 旧首烧包没有此字段，按关闭解释；显式假FAIL只允许V2的电源模拟配置。
    mats_fail = manifest.get("mats_fail_test", False)
    if (type(mats_fail) is not bool
            or (mats_fail and (expected_board != "apollo-v2" or not manifest["power_simulation"]))):
        raise ValueError("MATS假FAIL配置无效，禁止带入B1或非模拟镜像")
    for name in FILES:
        if sha(directory / name) != manifest["sha256"][name]:
            raise ValueError(f"首烧包文件校验失败：{name}")
    image = (directory / "app.bin").read_bytes()
    package = (directory / "firmware.ota").read_bytes()
    record = validate_package(package, image, TARGETS[expected_board][1])
    if ((directory / "initial-record.bin").read_bytes() != record
            or (directory / "stable-4k.bin").read_bytes() != package + b"\xff" * (-len(package) % 4096)):
        raise ValueError("初始记录或恢复包与App不匹配")
    version = ".".join(str(v) for v in struct.unpack_from("<3H", package, 44))
    if manifest.get("version") != version:
        raise ValueError("清单版本不匹配")
    boot = (directory / "boot.bin").read_bytes()
    if not 8 <= len(boot) <= 0x20000:
        raise ValueError("B区镜像长度非法")
    sp, entry = struct.unpack_from("<2I", boot)
    if sp != 0x20020000 or not entry & 1 or not 0x08000008 <= entry - 1 < 0x08000000 + len(boot):
        raise ValueError("B区向量非法")
    if TARGETS[expected_board][1].encode() + b"\0" not in boot:
        raise ValueError("不是对应板型的完整B")
    sp, entry = struct.unpack_from("<2I", image)
    if (sp % 8 or not (0x20000000 < sp <= 0x20020000 or 0x24000000 < sp <= 0x24080000)
            or not entry & 1 or not A1 + 8 <= entry - 1 < A1 + len(image)):
        raise ValueError("A1向量非法")
    if expected_board == "b1" and manifest.get("power_simulation") is not False:
        raise ValueError("B1不允许电源模拟")
    return manifest


def check_elf(compiler: Path, elf: Path, base: int, size: int, binary: Path | None = None) -> None:
    """检查每个LOAD节的物理装载地址；可选逐字节核对ELF与bin，拒绝跨区写入。"""
    result = subprocess.run([str(compiler.with_name("arm-none-eabi-objdump.exe")), "-h", str(elf)],
                            check=True, capture_output=True, text=True)
    lines = result.stdout.splitlines()
    loaded = []
    for index, line in enumerate(lines[:-1]):
        match = re.match(r"\s*\d+\s+\S+\s+([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+", line)
        if match and "LOAD" in lines[index + 1]:
            count, address = (int(value, 16) for value in match.groups())
            if count:
                if address < base or address + count > base + size:
                    raise ValueError(f"ELF装载范围越界：{elf.name}")
                loaded.append(address)
    if not loaded or min(loaded) != base:
        raise ValueError(f"ELF起始地址错误：{elf.name}")
    if binary is not None:
        # 输出到独立临时目录，不覆盖交付物；用于检测ELF/bin混搭。
        import tempfile
        with tempfile.TemporaryDirectory(prefix="first-flash-check-") as temp:
            converted = Path(temp) / "image.bin"
            run(compiler.with_name("arm-none-eabi-objcopy.exe"), "-O", "binary", elf, converted)
            if converted.read_bytes() != binary.read_bytes():
                raise ValueError(f"ELF与bin不一致：{elf.name}")


def build_bundle(args) -> Path:
    """复用双槽核验与打包器；输出固定包，烧写时不再编译，便于同事复现版本。"""
    app, _ = TARGETS[args.board]
    if args.power_simulation and args.board != "apollo-v2":
        raise ValueError("只有V2可显式选择模拟电源，RK仍真实")
    if args.mats_fail_test and (args.board != "apollo-v2" or not args.power_simulation):
        raise ValueError("MATS假FAIL仅用于V2电源模拟镜像")
    if not re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", args.version) or any(
            int(part) > 65535 for part in args.version.split(".")):
        raise ValueError("版本须为三个0..65535整数")
    app_root = args.app_root.resolve()
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    variant = (f"uart{args.rk_uart}" if app == "b1" else
               "power-sim" if args.power_simulation else "real")
    if args.mats_fail_test:
        variant += "-mats-fail"
    output = app_root / "build/first-flash" / f"{args.board}-{args.version}-{variant}-{stamp}"
    boot_build = ROOT / "build" / f"first-flash-{args.board}-{args.configuration.lower()}"
    env = os.environ
    env["PATH"] = str(args.compiler.parent) + os.pathsep + env.get("PATH", "")
    env.setdefault("SOURCE_DATE_EPOCH", str(int(time.time())))
    verifier = ROOT.parent / "test/tests/verify_ota_relocation.py"
    run(sys.executable, "-B", "-X", "utf8", verifier, "--app-root", app_root,
        "--app-name", app, "--ota-boot", "--version", args.version,
        "--rk-uart", args.rk_uart, "--build-type", args.configuration,
        "--compiler", args.compiler, "--cmake", args.cmake, "--ninja", args.ninja,
        *(["--power-simulation"] if args.power_simulation else []),
        *(["--mats-fail-test"] if args.mats_fail_test else []))
    prefix = "ota-app-power-sim" if args.power_simulation else "ota-app"
    if args.mats_fail_test:
        prefix += "-mats-fail"
    suffix = "-uart2" if app == "b1" and args.rk_uart == 2 else ""
    app_build = app_root / "build" / f"{prefix}-{args.version}-{args.configuration.lower()}-a1{suffix}"
    cache = (app_build / "CMakeCache.txt").read_text(encoding="utf-8")
    if "ENABLE_OTA_BOOT:BOOL=ON" not in cache or re.search(
            r"^ENABLE_(?:RK_GATEWAY|FLASH_SELF|DRIVER_MOCK|AD7124_WAVE|PWR_WAVE|RESULT_LED_WAVE|PC_PROTOCOL_NET)_TEST:BOOL=ON$", cache, re.M):
        raise ValueError("App不是OTA正式任务配置，拒绝交付测试镜像")
    if app == "test":
        for option, enabled in (("ENABLE_V2_POWER_SIMULATION", args.power_simulation),
                                ("ENABLE_V2_MATS_FAIL_TEST", args.mats_fail_test)):
            if not re.search(rf"^{option}:BOOL={'ON' if enabled else 'OFF'}$", cache, re.M):
                raise ValueError(f"构建配置与首烧清单不符：{option}")
    run(sys.executable, "-B", "-X", "utf8", ROOT / "tools/pack_ota.py", "--app-build", app_build,
        "--app-name", app, "--compiler", args.compiler)
    run(args.cmake, "-S", ROOT, "-B", boot_build, "-G", "Ninja",
        f"-DCMAKE_BUILD_TYPE={args.configuration}", f"-DCMAKE_MAKE_PROGRAM={args.ninja}",
        f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'cmake/gcc-arm-none-eabi.cmake'}",
        f"-DBOOT_BOARD={args.board}", "-DENABLE_OTA_RELOCATION_PROBE=OFF",
        "-DENABLE_BOOT_RAM_AGENT=ON", "-DENABLE_OTA_CORE_TEST=OFF")
    run(args.cmake, "--build", boot_build, "--target", "boot", "boot_ram_agent", "--parallel", "8")
    check_elf(args.compiler, boot_build / "boot.elf", 0x08000000, 0x20000, boot_build / "boot.bin")
    check_elf(args.compiler, app_build / f"{app}.elf", A1, pack.IMAGE_LIMIT, app_build / f"{app}.bin")
    check_elf(args.compiler, boot_build / "boot_ram_agent.elf", 0x24000000, 0x80000)
    output.mkdir(parents=True, exist_ok=False)
    for name in ("boot.elf", "boot.bin", "boot_ram_agent.elf"):
        shutil.copy2(boot_build / name, output / name)
    for extension in ("elf", "bin"):
        shutil.copy2(app_build / f"{app}.{extension}", output / f"app.{extension}")
    shutil.copy2(app_build / f"{app}.ota", output / "firmware.ota")
    for name in ("first_flash.py", "board_ota.py", "pack_ota.py", "flash.ps1"):
        shutil.copy2(ROOT / "tools" / name, output / name)
    shutil.copy2(ROOT / "tests/prepare_board_ota.py", output / "prepare_board_ota.py")
    package, image = (output / "firmware.ota").read_bytes(), (output / "app.bin").read_bytes()
    (output / "initial-record.bin").write_bytes(validate_package(package, image, TARGETS[args.board][1]))
    (output / "stable-4k.bin").write_bytes(package + b"\xff" * (-len(package) % 4096))
    manifest = {"format": 1, "board": args.board, "version": args.version,
                "configuration": args.configuration, "power_simulation": args.power_simulation,
                "mats_fail_test": args.mats_fail_test,
                "rk_uart": args.rk_uart if app == "b1" else 1,
                "sha256": {name: sha(output / name) for name in FILES}}
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    (output / "README.txt").write_text(
        f"板型={args.board}，版本={args.version}，电源模拟={args.power_simulation}，RK不模拟。\n"
        f"MATS第一颗假FAIL={args.mats_fail_test}；仅V2联调可开，第二颗保留RK结果。\n"
        "本包仅支持空板首部署，不支持覆盖已有程序或OTA日志。整个目录一起转交，不要单独选bin。\n"
        "需要Windows、Python 3.10+及STM32Cube的GNU Arm/GDB Server/Programmer工具；无需源码或重编译。\n"
        "先运行 flash.ps1 -Action verify；确认RK/温控空闲并断开网络助手后执行：\n"
        f"powershell -ExecutionPolicy Bypass -File .\\flash.ps1 -Board {args.board} -Serial <ST-Link序列号> -ConfirmIdle YES -ConfirmBlank YES\n"
        "路径不可含空格/引号。备份和报告存records目录。失败不得反复重刷，不会自动擦除旧数据。\n"
        "首次恢复源是本包App，稳定性仍需硬件验收；FRU保留本板数据，不克隆身份。\n"
        "当前IP为本地测试配置192.168.1.201，多板同网需先解决地址冲突；B1正式IP由B2通知，接口另行冻结。\n"
        "V2当前MAC也固定为00:80:E1:00:00:00；同一广播域暂时只接一块这种V2，不能只改IP就并用。\n"
        "首次烧录成功后使用日常一键SWD或网络OTA更新，不重复首烧。\n", encoding="utf-8-sig")
    validate_bundle(output, args.board)
    archive = shutil.make_archive(str(output), "zip", root_dir=output)
    print(f"PASS：首烧包已构建并离线校验：{output}\n可转交同事：{archive}\n"
          "未连接ST-Link；新首烧编排尚待实板验收。", flush=True)
    return output


def require_blank(internal: bytes, nor: bytes) -> None:
    """仅允许空内部Flash和空管理/恢复区；FRU与下载区不覆盖，不接受force绕过。"""
    if len(internal) != 0x200000 or len(nor) != NOR_BYTES:
        raise ValueError("备份长度错误")
    for name, data in (("内部Flash", internal), ("版本区", nor[:0x2000]),
                       ("启动日志", nor[0x4000:0x6000]), ("稳定区", nor[STABLE:NOR_BYTES])):
        if data != b"\xff" * len(data):
            raise ValueError(f"{name}不是全空，拒绝首烧。备份已保留，请另行确认迁移/恢复方案。")


def verify_internal(data: bytes, bundle: Path) -> None:
    """完整比较2MiB内部Flash，确认只写B/A1，A2和其余空间保持FF。"""
    expected = bytearray(b"\xff" * 0x200000)
    for name, offset in (("boot.bin", 0), ("app.bin", 0x20000)):
        image = (bundle / name).read_bytes()
        expected[offset:offset + len(image)] = image
    if data != expected:
        raise ValueError("内部Flash回读不符，禁止提交启动记录")


def flash_bundle(args) -> None:
    """备份→空板检查→B/A1→恢复包→启动日志→健康确认；不发送任何RK/PC命令。"""
    bundle = args.bundle.resolve()
    validate_bundle(bundle, args.board)
    if not args.confirm_idle or not args.confirm_blank or not re.fullmatch(r"[0-9a-fA-F]{24}", args.serial):
        raise ValueError("必须明确24位ST-Link序列号，并确认空板、RK/温控空闲及允许复位")
    if any(c.isspace() or c == '"' for c in str(bundle)):
        raise ValueError("现有GDB工具要求首烧包路径不含空白或引号")
    # 在接触板卡前验证ELF真实装载范围；散列本身不足以证明地址正确。
    check_elf(args.compiler, bundle / "boot.elf", 0x08000000, 0x20000, bundle / "boot.bin")
    check_elf(args.compiler, bundle / "app.elf", A1, pack.IMAGE_LIMIT, bundle / "app.bin")
    check_elf(args.compiler, bundle / "boot_ram_agent.elf", 0x24000000, 0x80000)
    with socket.socket() as check:
        check.bind(("127.0.0.1", 61234))
    directory = bundle / "records" / (datetime.now().strftime("%Y%m%d-%H%M%S-%f") + "-" + args.serial)
    directory.mkdir(parents=True, exist_ok=False)
    board.GNU, board.AGENT_ELF = args.compiler.parent, bundle / "boot_ram_agent.elf"
    connect = ["set pagination off", "set confirm off", "set remotetimeout 20",
               "target extended-remote localhost:61234"]
    report = {"board": args.board, "serial": args.serial, "result": "incomplete", "stage": "connect"}
    server = None
    started = False

    def action(kind: str, name: str, *extra: object) -> None:
        board.main([str(v) for v in (kind, "--directory", directory, "--name", name, *extra)])

    try:
        with (directory / "stlink-server.log").open("w", encoding="utf-8") as log:
            server = subprocess.Popen([str(args.server), "-e", "-d", "-g", "-p", "61234",
                                       "-i", args.serial, "-cp", str(args.programmer.parent)],
                                      stdout=log, stderr=subprocess.STDOUT,
                                      creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            deadline = time.monotonic() + 20
            while "Waiting for debugger connection" not in (directory / "stlink-server.log").read_text(errors="replace"):
                if server.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("ST-Link服务器未就绪；不终止其他调试器")
                time.sleep(0.2)
        report["stage"] = "backup"
        print("备份内部2MiB与W25前0x410000字节；检查空板，不修改选项字节。", flush=True)
        board.run_script(directory, "internal-backup", connect + ["monitor reset", "monitor halt",
            "if ((*(unsigned int*)0x5C001000 & 0xFFF) != 0x450) || (*(unsigned short*)0x1FF1E880 != 2048)",
            "quit 2", "end",
            "if (*(unsigned int*)0x5200201C & 0x8000FF00) != 0x0000AA00", "quit 2", "end",
            f"dump binary memory {directory.as_posix()}/original-internal.bin 0x08000000 0x08200000"])
        action("backup", "nor-backup")
        original = (directory / "original-internal.bin").read_bytes()
        nor = (directory / "original-w25-0-410000.bin").read_bytes()
        require_blank(original, nor)
        report["stage"] = "internal-program"
        commands = connect + ["monitor reset", "monitor halt"]
        for name in ("boot.elf", "app.elf"):
            commands += [f'file "{(bundle / name).as_posix()}"', "load"]
        # load会设置入口PC；重新进入纯RAM助手，不能让尚无启动记录的App提前运行。
        commands += board.load_agent()
        commands += [f"dump binary memory {directory.as_posix()}/installed-internal.bin 0x08000000 0x08200000"]
        board.run_script(directory, "install-internal", commands)
        verify_internal((directory / "installed-internal.bin").read_bytes(), bundle)
        report["stage"] = "stable-package"
        action("write", "write-stable", "--address", hex(STABLE), "--input", bundle / "stable-4k.bin")
        action("read", "verify-nor", "--address", 0, "--length", NOR_BYTES)
        expected_nor = bytearray(nor)
        stable = (bundle / "stable-4k.bin").read_bytes()
        expected_nor[STABLE:STABLE + len(stable)] = stable
        if (directory / "verify-nor.bin").read_bytes() != expected_nor:
            raise ValueError("W25回读不一致或非目标区发生变化，禁止提交启动记录")
        report["stage"] = "initial-record"
        # C接口会编码256字节持久化页、最后写提交标记并回读，不能直接烧236字节RAM结构。
        action("record", "commit-initial", "--input", bundle / "initial-record.bin")
        report["stage"] = "health"
        board.run_script(directory, "start-app", connect + ["monitor reset"])
        started = True
        print("首版A1已启动，等待30秒连续健康确认；不会自动重试烧写。", flush=True)
        deadline = time.monotonic() + 180
        attempt = 0
        while time.monotonic() < deadline:
            time.sleep(5)
            attempt += 1
            board.run_script(directory, f"health-{attempt}", [f'file "{(bundle / "app.elf").as_posix()}"'] + connect + [
                "monitor halt", f"dump binary memory {directory.as_posix()}/vtor.bin 0xE000ED08 0xE000ED0C",
                f"dump binary memory {directory.as_posix()}/health.bin &ota_app_diag ((char*)&ota_app_diag+32)"])
            diag = struct.unpack("<8I", (directory / "health.bin").read_bytes())
            vtor = struct.unpack("<I", (directory / "vtor.bin").read_bytes())[0]
            if vtor != A1 or diag[0] != 0x4F544131 or diag[1] != A1 or diag[2] != 0:
                raise ValueError("启动槽/健康状态异常，停止首烧，不强行写成功标志")
            if diag[3] not in (0, 1):
                raise ValueError("首启已出现额外复位，停止检查并保留现场")
            if diag[3] == 1 and diag[4] == 15 and diag[5] >= 30000:
                report.update(result="passed", stage="complete", health=list(diag), vtor=hex(vtor))
                print(f"PASS：B/A1与恢复包回读、保护区不变、四任务30秒健康确认。记录：{directory}", flush=True)
                break
        else:
            raise TimeoutError("180秒内未健康确认；保留备份，不自动重刷/重启")
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        if server is not None and server.poll() is None:
            try:
                # 成功启动后绝不再次reset；失败时退出RAM助手，交由B的CRC/维护门禁处理。
                board.run_script(directory, "release-probe", connect + ([] if started else ["monitor reset"]), timeout=30)
            except Exception as error:
                report["release_warning"] = str(error)
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)
        (directory / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("build", "deploy", "flash", "verify"))
    parser.add_argument("--board", choices=TARGETS, required=True)
    parser.add_argument("--app-root", type=Path)
    parser.add_argument("--version", default="")
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Release")
    parser.add_argument("--rk-uart", choices=(2, 4), type=int, default=4)
    parser.add_argument("--power-simulation", action="store_true")
    parser.add_argument("--mats-fail-test", action="store_true")
    parser.add_argument("--bundle", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--serial", default="")
    parser.add_argument("--confirm-idle", action="store_true")
    parser.add_argument("--confirm-blank", action="store_true")
    for name in ("compiler", "cmake", "ninja", "server", "programmer"):
        parser.add_argument("--" + name, type=Path)
    args = parser.parse_args()
    needs_build = args.action in ("build", "deploy")
    needs_flash = args.action in ("flash", "deploy")
    required = ("compiler", "cmake", "ninja", "app_root") if needs_build else ()
    if needs_flash:
        required += ("compiler", "server", "programmer")
    if any(getattr(args, name) is None for name in required):
        parser.error("缺少本操作所需的工程/工具路径：" + ", ".join(required))
    if not needs_build and (args.version or args.power_simulation or args.mats_fail_test):
        parser.error("烧写/校验使用包内固定配置，不接受重新指定版本或模拟开关")
    if needs_build and args.board == "b1" and (args.power_simulation or args.mats_fail_test or args.rk_uart != 4):
        parser.error("B1构建仅允许UART4且禁止V2模拟/假FAIL")
    if args.mats_fail_test and not args.power_simulation:
        parser.error("MATS假FAIL必须显式配合V2电源模拟")
    if needs_flash and (not args.confirm_idle or not args.confirm_blank
                        or not re.fullmatch(r"[0-9a-fA-F]{24}", args.serial)):
        parser.error("须指定24位ST-Link序列号，确认空板与RK/温控空闲；未确认前不构建或连接")
    if needs_build:
        # 只烧本次构建且完整校验的目录；构建异常直接退出，不落回旧产物。
        args.bundle = build_bundle(args)
    if needs_flash:
        flash_bundle(args)
    elif args.action == "verify":
        print(json.dumps(validate_bundle(args.bundle.resolve(), args.board), ensure_ascii=False, indent=2))
        print("PASS：首烧包完整性与板型校验通过；未连接设备。")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        raise SystemExit(f"FAIL：{error}") from error
