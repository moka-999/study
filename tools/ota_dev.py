#!/usr/bin/env python3
"""V2日常构建/下载入口；复用正式OTA链路，绝不直接覆盖内部App或B。

build不访问板卡；network经App下载；swd经RAM助手写W25下载区，由B安装。
仅适用于已部署完整OTA的阿波罗V2；不是空片首烧或B1量产工具。
"""
from __future__ import annotations

import argparse
from datetime import datetime
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
BOOT = ROOT.parent / "bootloader"
SLOTS = (0x08020000, 0x08100000)
SLOT_BYTES = 896 * 1024
# 新版四任务位图0x0F；旧版含独立心跳任务为0x1F，仅兼容这两种完整组合。
HEALTH_TASK_MASKS = (0x0F, 0x1F)
PYTHON = [sys.executable, "-B", "-X", "utf8"]


def run(*command: object) -> None:
    """子步骤失败即停止；不重试任何擦写或提交操作。"""
    subprocess.run([str(part) for part in command], check=True)


def validate_request(record: bytes, internal: bytes, package: bytes) -> int:
    """核对既有正常镜像及新版本，返回待安装槽0/1；record由现成日志工具生成。"""
    if len(record) != 236 or len(internal) != 0x200000 or not 116 <= len(package) <= 0x200000:
        raise ValueError("启动请求/内部备份/升级包长度错误")
    if (package[:4] != b"OTA1" or struct.unpack_from("<I", package, 12)[0] != len(package)
            or struct.unpack_from("<I", package, 36)[0] != zlib.crc32(package[:36])
            or package[100:116] != b"apollo-v2-h743".ljust(16, b"\0")):
        raise ValueError("升级包头/目标板型不符")
    fields = struct.unpack_from("<13I", record)
    active = fields[2]
    if (fields[1] != 1 or active not in (0, 1) or not fields[4] & (1 << active)
            or fields[5] != 0 or fields[6] >= 2 or fields[8] != 0
            or fields[9] != len(package) or fields[10] != zlib.crc32(package[:36])):
        raise ValueError("仅允许已确认、非稳定恢复阶段的完整OTA板卡升级")
    offset = 52 + active * 92
    base, size, crc, _ = struct.unpack_from("<4I", record, offset)
    version = record[offset + 16:offset + 92]
    if (base != SLOTS[active] or not 8 <= size <= SLOT_BYTES
            or version[:4] != b"\x01\x00\x4c\x00"
            or version[60:76] != b"apollo-v2-h743".ljust(16, b"\0")
            or struct.unpack_from("<2I", version, 12) != (size, crc)):
        raise ValueError("当前槽描述或板型错误；本入口只支持V2，不初始化/重建日志")
    start = base - 0x08000000
    if zlib.crc32(internal[start:start + size]) != crc:
        raise ValueError("当前App与启动日志CRC不符，禁止在异常基线上继续升级")
    current = struct.unpack_from("<3H", version, 4)
    incoming = struct.unpack_from("<3H", package, 44)
    if incoming <= current:
        raise ValueError(f"新版本{incoming}必须大于当前{current}；同版本调试也请递增patch")
    return 1 - active


def validate_running(internal: bytes, original: bytes, image: bytes,
                     slot: int, diag: bytes, vtor: bytes) -> None:
    """用本次ELF对应的镜像/诊断验证安装，禁止只凭串口文字判定成功。"""
    if len(internal) != 0x200000 or len(original) != 0x200000 or len(diag) != 32 or len(vtor) != 4:
        raise ValueError("安装回读不完整")
    start = SLOTS[slot] - 0x08000000
    state = struct.unpack("<8I", diag)
    if (struct.unpack("<I", vtor)[0] != SLOTS[slot] or state[:3] != (0x4F544131, SLOTS[slot], 0)
            or state[4] not in HEALTH_TASK_MASKS or state[5] == 0):
        raise ValueError("新App尚未持续健康30秒，或当前运行槽/健康状态错误")
    if internal[start:start + len(image)] != image:
        raise ValueError("实际App与本次构建不一致")
    if internal[:start] != original[:start] or internal[start + SLOT_BYTES:] != original[start + SLOT_BYTES:]:
        raise ValueError("非目标槽发生变化：B、原运行槽及保留区应保持不变")


def build_package(args: argparse.Namespace) -> Path:
    """两个链接地址只用于一致性验证；最终发布仍是一个test.ota。"""
    print("构建槽限App并验证A1/A2地址修正；此阶段不连接ST-Link。", flush=True)
    run(*PYTHON, ROOT / "tests/verify_ota_relocation.py", "--cmake", args.cmake,
        "--ninja", args.ninja, "--compiler", args.compiler, "--ota-boot",
        "--version", args.version, "--build-type", args.configuration,
        *(["--power-simulation"] if args.power_simulation else []),
        *(["--mats-fail-test"] if args.mats_fail_test else []))
    prefix = "ota-app-power-sim" if args.power_simulation else "ota-app"
    if args.mats_fail_test:
        prefix += "-mats-fail"
    build = ROOT / "build" / f"{prefix}-{args.version}-{args.configuration.lower()}-a1"
    run(*PYTHON, BOOT / "tools/pack_ota.py", "--app-build", build, "--compiler", args.compiler)
    return build


def swd_download(args: argparse.Namespace, build: Path, directory: Path) -> None:
    """独占本次服务器，备份→检查→下载区写入/回读→C日志提交→B安装→健康确认。"""
    sys.path.insert(0, str(BOOT / "tools"))
    import board_ota as board

    board.GNU = args.compiler.parent
    if any(char.isspace() or char == '"' for char in str(directory)):
        raise ValueError("现有SWD助手要求工程/备份路径不含空白或引号")
    # 该助手固定使用61234；端口被占用时退出，绝不终止其他调试器进程。
    with socket.socket() as check:
        check.bind(("127.0.0.1", 61234))
    run(args.cmake, "-S", BOOT, "-B", BOOT / "build/Debug", "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Debug", f"-DCMAKE_TOOLCHAIN_FILE={BOOT / 'cmake/gcc-arm-none-eabi.cmake'}",
        f"-DCMAKE_MAKE_PROGRAM={args.ninja}", "-DBOOT_BOARD=apollo-v2",
        "-DENABLE_BOOT_RAM_AGENT=ON", "-DENABLE_OTA_RELOCATION_PROBE=OFF")
    run(args.cmake, "--build", BOOT / "build/Debug", "--target", "boot_ram_agent", "--parallel", "8")

    def board_action(action: str, name: str, *extra: object) -> None:
        # CLI校验本轮完整备份；仍由C接口实现NOR擦写及日志提交。
        board.main([str(part) for part in (action, "--directory", directory, "--name", name, *extra)])

    connect = ["set pagination off", "set confirm off", "set remotetimeout 20",
               "target extended-remote localhost:61234"]
    needs_reset = False
    server = None
    with (directory / "stlink-server.log").open("w", encoding="utf-8") as log:
        try:
            server = subprocess.Popen([str(args.server), "-e", "-d", "-g", "-p", "61234",
                                       "-i", args.serial, "-cp", str(args.programmer.parent)],
                                      stdout=log, stderr=subprocess.STDOUT,
                                      creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            deadline = time.monotonic() + 20
            while "Waiting for debugger connection" not in (directory / "stlink-server.log").read_text(errors="replace"):
                if server.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("ST-Link连接失败/被占用，见stlink-server.log；未终止其他调试器")
                time.sleep(0.2)
            print("检查V2电源输出为低，备份内部Flash和W25相关区域；期间不要拔电。", flush=True)
            original = directory / "original-internal.bin"
            # 检查GPIO实际处于输出低；这里只拒绝，不替用户强制切断正在测试的RK。
            board.run_script(directory, "internal-backup", connect + ["monitor halt",
                "if ((*(unsigned int*)0x58020800 & 0xF000) != 0x5000) || ((*(unsigned int*)0x58021800 & 0xC00000) != 0x400000)",
                'echo PC6/PC7/PG11 are not power-control outputs.\\n', "quit 2", "end",
                "if (*(unsigned int*)0x58020814 & 0xC0) || (*(unsigned int*)0x58021814 & 0x800)",
                'echo Power is not OFF; stop the test before downloading.\\n', "quit 2", "end",
                f"dump binary memory {original.as_posix()} 0x08000000 0x08200000"])
            needs_reset = True
            board_action("backup", "nor-backup")
            run(*PYTHON, BOOT / "tests/prepare_board_ota.py", "--app-build", build,
                "--directory", directory, "--name", "upgrade", "--journal", directory / "original-w25-0-410000.bin")
            package = (build / "test.ota").read_bytes()
            slot = validate_request((directory / "upgrade-record.bin").read_bytes(), original.read_bytes(), package)
            board_action("write", "stage-package", "--address", "0x10000", "--input", directory / "upgrade-package-4k.bin")
            board_action("read", "stage-readback", "--address", "0x10000", "--length", (directory / "upgrade-package-4k.bin").stat().st_size)
            if (directory / "stage-readback.bin").read_bytes() != (directory / "upgrade-package-4k.bin").read_bytes():
                raise ValueError("W25完整升级包回读不一致，未提交安装请求")
            board_action("record", "commit-ready", "--input", directory / "upgrade-record.bin")
            board.run_script(directory, "start-installer", connect + ["monitor reset"])
            needs_reset = False
            print(f"B正在安装到A{slot + 1}，等待App健康确认（至少30秒）。", flush=True)
            elf = build.parent / (build.name[:-2] + f"a{slot + 1}") / "test.elf"
            image = (build / ("test.bin" if slot == 0 else "a2_from_a1.bin")).read_bytes()
            deadline = time.monotonic() + 180
            attempt = 0
            while time.monotonic() < deadline:
                time.sleep(5)
                attempt += 1
                diag, vtor = directory / "health.bin", directory / "vtor.bin"
                actual = directory / "installed-internal.bin"
                # 符号来自本次独立链接的对应槽ELF；file只加载符号，不load Flash。
                board.run_script(directory, f"health-{attempt}", [f'file "{elf.as_posix()}"'] + connect + [
                    "monitor halt", f"dump binary memory {vtor.as_posix()} 0xE000ED08 0xE000ED0C",
                    f"dump binary memory {diag.as_posix()} &ota_app_diag ((char*)&ota_app_diag+32)",
                    f"if (*(unsigned int*)0xE000ED08 == {SLOTS[slot]}) && ota_app_diag[0] == 0x4F544131 && ota_app_diag[1] == {SLOTS[slot]} && ota_app_diag[2] == 0 && ota_app_diag[5] != 0",
                    f"dump binary memory {actual.as_posix()} 0x08000000 0x08200000", "end"], timeout=30)
                if actual.exists():
                    validate_running(actual.read_bytes(), original.read_bytes(), image, slot, diag.read_bytes(), vtor.read_bytes())
                    print(f"PASS：A{slot + 1}安装及30秒健康确认；B、原运行槽和保留区未变。\n调试符号：{elf}", flush=True)
                    return
                print(f"尚未健康确认，第{attempt}次检查（超时不重发安装请求）。", flush=True)
            raise RuntimeError("目标App未健康确认，可能安装失败或已回滚；保留日志，不强制重刷")
        finally:
            if server is not None:
                try:
                    if server.poll() is None:
                        # 写入中断也交由原B按可靠日志恢复；不强行写成功标志。
                        commands = connect + (["monitor reset"] if needs_reset else [])
                        board.run_script(directory, "release-probe", commands, timeout=30)
                except Exception as error:
                    print(f"警告：无法恢复/释放目标连接：{error}；关闭调试器后手动按RESET交由B恢复，勿重复覆盖Flash。", file=sys.stderr)
                finally:
                    if server.poll() is None:
                        server.terminate()
                        try:
                            server.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            server.kill()
                            server.wait(timeout=5)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("build", "swd", "network"))
    parser.add_argument("--version", required=True, help="三段版本号；必须大于板上版本")
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Debug")
    for name in ("cmake", "ninja", "compiler"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--server", type=Path)
    parser.add_argument("--programmer", type=Path)
    parser.add_argument("--serial", default="")
    parser.add_argument("--host", default="192.168.1.201")
    parser.add_argument("--port", type=int, default=8888)
    parser.add_argument("--legacy-ota-actions", action="store_true",
                        help="仅network旧版迁移：上传用旧动作，重启后先查mcu_query")
    parser.add_argument("--confirm-idle", action="store_true", help="操作者确认已停测试/温控，允许复位升级")
    parser.add_argument("--power-simulation", action="store_true", help="仅V2：模拟电源/测量，保留真实RK与其他正式任务")
    parser.add_argument("--mats-fail-test", action="store_true", help="仅V2电源模拟：每次真实MATS结束向PC上报模拟FAIL")
    args = parser.parse_args()
    if args.mats_fail_test and not args.power_simulation:
        parser.error("--mats-fail-test必须配合--power-simulation，禁止用于普通固件")
    if args.legacy_ota_actions and args.action != "network":
        parser.error("--legacy-ota-actions仅用于network升级旧版固件")
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", args.version) or any(int(n) > 65535 for n in args.version.split(".")):
        parser.error("版本须为三段0..65535整数，例如0.1.9")
    if args.action == "swd" and (not re.fullmatch(r"[0-9A-Fa-f]{24}", args.serial) or not args.server or not args.programmer):
        parser.error("SWD需要明确的24位ST-Link序列号、服务器和Programmer路径")
    if not 1 <= args.port <= 65535:
        parser.error("端口须在1..65535")
    if args.action != "build" and not args.confirm_idle:
        parser.error("先停止测试并独立协调B2温控，再用--confirm-idle明确允许复位；本工具不会关闭B2")
    os.environ["PATH"] = str(args.compiler.parent) + os.pathsep + os.environ.get("PATH", "")
    # A1/A2使用同一次构建时间，既准确记录版本信息，又能逐字节比较地址修正。
    os.environ.setdefault("SOURCE_DATE_EPOCH", str(int(time.time())))
    build = build_package(args)
    print(f"升级文件：{build / 'test.ota'}", flush=True)
    if args.action == "build":
        return
    directory = ROOT / "build/ota-dev" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    directory.mkdir(parents=True, exist_ok=False)
    print(f"本轮备份/日志：{directory}", flush=True)
    if args.action == "network":
        run(*PYTHON, ROOT / "tests/pc_ota_network_test.py", "--host", args.host, "--port", args.port,
            "--package", build / "test.ota", "--upgrade", "--report", directory / "network.json",
            *(["--legacy-ota-actions"] if args.legacy_ota_actions else []))
    else:
        swd_download(args, build, directory)


if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        raise SystemExit(f"FAIL：{error}") from error
