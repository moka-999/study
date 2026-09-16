#!/usr/bin/env python3
"""编译并在已安装QEMU中执行真实C安装核心；没有STM32/QSPI电气仿真。

Python只制造输入/故障条件/逐字节答案，所有校验、修正、安装决策由C执行。
失败立即退出，保留输入、日志与报告；不烧录、不访问网络、不修改App源码。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import pack_ota as pack


def invoke(command: list[str], *, cwd: Path, env: dict[str, str], timeout: int = 45) -> str:
    """限制每个子进程时间；QEMU异常/超时绝不当成PASS。"""
    process = subprocess.run(command, cwd=cwd, env=env, capture_output=True,
                             text=True, encoding="utf-8", errors="replace", timeout=timeout)
    output = process.stdout + process.stderr
    if process.returncode:
        raise ValueError(f"命令失败({process.returncode}): {' '.join(command)}\n{output}")
    return output


def fixture(length: int = 581, offsets: list[int] | None = None, sp: int = 0x20020000):
    """构造跨32B/256B边界以及不满最后Flash-word的源镜像；不是可运行App。"""
    offsets = offsets or [4, 31, 253, 259, 511, 577]
    image = bytearray((i * 13 + 7) & 255 for i in range(length))
    struct.pack_into("<I", image, 0, sp)
    for i, offset in enumerate(offsets):
        struct.pack_into("<I", image, offset, pack.A1_BASE + (9 if length < 66 else 65) + i * 2)
    # 一个恰好像Flash地址、但不是ELF地址项的数据，必须原样保留。
    if length > 104:
        struct.pack_into("<I", image, 100, pack.A1_BASE + 100)
    a2 = bytearray(image)
    for offset in offsets:
        value = struct.unpack_from("<I", a2, offset)[0]
        struct.pack_into("<I", a2, offset, value + pack.A2_BASE - pack.A1_BASE)
    version = struct.pack("<HHHHH2xII24s16s16s", 1, 76, 0, 2, 0, length,
                          zlib.crc32(image), b"2026-09-15T00:00:00Z", b"test", b"apollo-v2-h743")
    return pack.build_package(image, version, offsets, a2), bytes(image), bytes(a2)


def repair_header(data: bytearray) -> bytes:
    """故意为畸形字段重算CRC，证明C没有仅靠CRC放行越界输入。"""
    struct.pack_into("<I", data, 28, zlib.crc32(data[40:116]))
    struct.pack_into("<I", data, 36, zlib.crc32(data[:36]))
    return bytes(data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--ninja", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=Path("D:/qemu/qemu-system-arm.exe"))
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Debug")
    parser.add_argument("--app-build", type=Path, action="append", default=[])
    args = parser.parse_args()
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    env["PATH"] = str(args.compiler.parent) + os.pathsep + env.get("PATH", "")
    build = ROOT / "build" / f"{args.configuration}-ota-test"
    build.mkdir(parents=True, exist_ok=True)
    if not args.qemu.is_file():
        raise ValueError("没有找到QEMU，请传--qemu；不能仅编译就声称已运行C测试")
    output = invoke([str(args.cmake), "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
                     f"-DCMAKE_BUILD_TYPE={args.configuration}",
                     f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'cmake/gcc-arm-none-eabi.cmake'}",
                     f"-DCMAKE_MAKE_PROGRAM={args.ninja}", "-DENABLE_OTA_CORE_TEST=ON"], cwd=ROOT, env=env)
    output += invoke([str(args.cmake), "--build", str(build), "--target", "ota_core_test", "boot_record_test", "boot_main_test", "boot_main_b1_test"], cwd=ROOT, env=env)
    (build / "build.log").write_text(output, encoding="utf-8")
    print("C安装核心编译通过，开始执行故障注入。", flush=True)
    journal_output = invoke([str(args.qemu), "-M", "mps2-an500", "-display", "none",
        "-monitor", "none", "-serial", "none", "-no-reboot",
        "-semihosting-config", "enable=on,target=native", "-kernel", str(build / "boot_record_test.elf")],
        cwd=build, env=env)
    (build / "journal.log").write_text(journal_output, encoding="utf-8")
    if "PASS: C boot journal power cuts and 3+3 recovery" not in journal_output:
        raise ValueError(journal_output)
    print(journal_output.strip(), flush=True)
    results = []

    def case(name: str, data: bytes, expected: bytes = b"", *, status: int = 0,
             mode: int = 0, index: int = 0, active: int = pack.A1_BASE,
             current: tuple[int, int, int] = (0, 0, 0), header_crc: int | None = None,
             declared_length: int | None = None, recovery: int = 0) -> None:
        directory = build / name
        directory.mkdir(exist_ok=True)
        (directory / "input.ota").write_bytes(data)
        (directory / "expected.bin").write_bytes(expected or b"\0")
        crc = struct.unpack_from("<I", data, 36)[0] if header_crc is None else header_crc
        config = struct.pack("<11I", len(data) if declared_length is None else declared_length,
                             crc, active, status, mode, index, *current, len(expected), recovery)
        (directory / "config.bin").write_bytes(config)
        command = [str(args.qemu), "-M", "mps2-an500", "-display", "none", "-monitor", "none",
                   "-serial", "none", "-no-reboot", "-semihosting-config", "enable=on,target=native",
                   "-kernel", str(build / "ota_core_test.elf"),
                   "-device", "loader,file=input.ota,addr=0x60000000,force-raw=on",
                   "-device", "loader,file=expected.bin,addr=0x60200000,force-raw=on",
                   "-device", "loader,file=config.bin,addr=0x60400000,force-raw=on"]
        output = invoke(command, cwd=directory, env=env)
        (directory / "run.log").write_text(output, encoding="utf-8")
        if "PASS: C installer and protected ranges" not in output:
            raise ValueError(f"{name}: 未收到C测试完成标记\n{output}")
        results.append({"case": name, "expected_status": status,
                        "package_sha256": hashlib.sha256(data).hexdigest(), "result": "PASS"})
        if len(results) % 10 == 0:
            print(f"已通过{len(results)}项，最近：{name}", flush=True)

    package, a1, a2 = fixture()
    # 运行正式启动管理器而非Python状态模型；稳定包故意用更低版本0.0.9。
    stable = bytearray(package)
    struct.pack_into("<HHH", stable, 44, 0, 0, 9)
    (build / "manager-input.ota").write_bytes(package)
    (build / "manager-stable.ota").write_bytes(repair_header(stable))
    manager_output = invoke([str(args.qemu), "-M", "mps2-an500", "-display", "none", "-monitor", "none",
        "-serial", "none", "-no-reboot", "-semihosting-config", "enable=on,target=native",
        "-kernel", str(build / "boot_main_test.elf"),
        "-device", "loader,file=manager-input.ota,addr=0x60000000,force-raw=on",
        "-device", "loader,file=manager-stable.ota,addr=0x60600000,force-raw=on"], cwd=build, env=env)
    (build / "manager.log").write_text(manager_output, encoding="utf-8")
    if "PASS: C boot manager" not in manager_output:
        raise ValueError(manager_output)
    print(manager_output.strip(), flush=True)
    # 相同正式启动管理器切成B1可信板型；仅替换包目标并重算CRC，其余流程相同。
    for filename, data in (("manager-b1.ota", package), ("stable-b1.ota", stable)):
        b1 = bytearray(data)
        b1[100:116] = b"b1-h743".ljust(16, b"\0")
        (build / filename).write_bytes(repair_header(b1))
    b1_output = invoke([str(args.qemu), "-M", "mps2-an500", "-display", "none", "-monitor", "none",
        "-serial", "none", "-no-reboot", "-semihosting-config", "enable=on,target=native",
        "-kernel", str(build / "boot_main_b1_test.elf"),
        "-device", "loader,file=manager-b1.ota,addr=0x60000000,force-raw=on",
        "-device", "loader,file=stable-b1.ota,addr=0x60600000,force-raw=on"], cwd=build, env=env)
    (build / "manager-b1.log").write_text(b1_output, encoding="utf-8")
    if "PASS: C boot manager" not in b1_output:
        raise ValueError(b1_output)
    print("B1: " + b1_output.strip(), flush=True)
    # 同时给发布工具留一个最小反例检查，不允许它封装混合版本或坏表。
    for image, version, offsets, converted in (
            (a1, package[40:115], [4], a2),
            (a1[:-1], package[40:116], [4], a2[:-1]),
            (a1, package[40:116], [4, 4], a2),
            (a1, package[40:116], [4, len(a1)], a2)):
        try:
            pack.build_package(image, version, offsets, converted)
        except ValueError:
            pass
        else:
            raise ValueError("发布工具未拒绝非法元数据/修正表")
    for active, answer in ((pack.A1_BASE, a2), (pack.A2_BASE, a1)):
        label = "to_a2" if active == pack.A1_BASE else "to_a1"
        case(label, package, answer, active=active)
        for index in range(1, 8):
            case(f"{label}_erase_{index}", package, answer, status=12, mode=2, index=index, active=active)
        for index in range(1, (len(a1) + 31) // 32 + 1):
            case(f"{label}_program_{index}", package, answer, status=13, mode=3, index=index, active=active)
    # 首次验证共有1次包头读取、6项表读取、3块镜像读取；覆盖前后两遍所有读取点。
    for index in range(1, 20):
        case(f"source_read_{index}", package, status=10, mode=4, index=index)
    for index in range(1, 4):
        case(f"internal_read_{index}", package, status=10, mode=8, index=index)
    case("journal_failure", package, status=11, mode=5)
    case("vector_readback_corrupt", package, status=14, mode=6)
    case("crc_readback_corrupt", package, status=14, mode=9)
    case("source_changed_after_validation", package, status=7, mode=7)
    case("same_version", package, status=5, current=(0, 2, 0))
    case("lower_version", package, status=5, current=(1, 0, 0))
    # 稳定源只允许B内部恢复入口使用，必须从独立W25区域读取并固定写A1。
    for active in (pack.A1_BASE, pack.A2_BASE):
        case(f"stable_from_{active:08x}", package, a1, active=active, current=(9, 0, 0), recovery=1)
    case("stable_crc_failure", package, status=7, recovery=1, header_crc=0)
    case("invalid_recovery_mode", package, status=1, recovery=2)
    case("patch_upgrade", package, a2, current=(0, 1, 65535))
    case("invalid_confirmed_slot", package, status=1, active=0x08000000)
    case("stale_request", package, status=7, header_crc=0)
    case("truncated", package[:-1], status=6)
    case("oversize_request", package, status=6, declared_length=pack.PACKAGE_LIMIT + 1)
    case("undersize_request", package, status=6, declared_length=39)

    # 包头/元数据/修正表的语义错误；即使重新计算包头CRC仍应拒绝。
    corruptions = [
        ("magic", 0, "I", 0, 2), ("format", 4, "H", 2, 2),
        ("header_size", 6, "H", 44, 2), ("minimum_boot", 8, "H", 2, 3),
        ("minimum_boot_zero", 8, "H", 0, 3),
        ("flags", 10, "H", 1, 2), ("total", 12, "I", len(package) + 1, 6),
        ("count_zero", 16, "I", 0, 6), ("count_overflow", 16, "I", 0xFFFFFFFF, 6),
        ("table_crc", 20, "I", 0, 7), ("a2_crc", 24, "I", 0, 7),
        ("reserved", 32, "I", 1, 2), ("schema", 40, "H", 2, 2),
        ("metadata_size", 42, "H", 80, 2), ("padding", 50, "H", 1, 2),
        ("image_oversize", 52, "I", pack.IMAGE_LIMIT + 1, 6),
        ("image_empty", 52, "I", 0, 6), ("image_crc", 56, "I", 0, 7),
        ("missing_reset", 116, "I", 8, 8), ("patch_sp", 116, "I", 0, 8),
        ("duplicate", 120, "I", 4, 8), ("overlap", 120, "I", 6, 8),
        ("unsorted", 124, "I", 8, 8), ("offset_overflow", 120, "I", 0xFFFFFFFF, 8),
    ]
    for name, offset, fmt, value, status in corruptions:
        bad = bytearray(package)
        struct.pack_into("<" + fmt, bad, offset, value)
        case(name, repair_header(bad), status=status)
    for name, begin, end, value, status in (
            ("wrong_board", 100, 116, b"b1-h743".ljust(16, b"\0"), 4),
            ("unterminated_target", 100, 116, b"x" * 16, 2),
            ("nonzero_text_tail", 84, 100, b"test\0x".ljust(16, b"\0"), 2)):
        bad = bytearray(package)
        bad[begin:end] = value
        case(name, repair_header(bad), status=status)
    for name, offset in (("bad_header_crc", 38), ("bad_metadata_crc", 84),
                         ("bad_image", len(package) - len(a1) + 100)):
        bad = bytearray(package)
        bad[offset] ^= 0x10
        case(name, bytes(bad), status=7)
    image_start = len(package) - len(a1)
    for name, offset, value, status in (
            ("ram_gap", 0, 0x20030000, 9), ("stack_unaligned", 0, 0x2001FFFC, 9),
            ("stack_bottom", 0, 0x20000000, 9), ("even_reset", 4, pack.A1_BASE + 64, 9),
            ("outside_reset", 4, pack.A1_BASE + 1001, 9),
            ("last_byte_reset", 4, pack.A1_BASE + 581, 9),
            ("relocation_ram", 31, 0x20000100, 8)):
        bad = bytearray(package)
        struct.pack_into("<I", bad, image_start + offset, value)
        case(name, bytes(bad), status=status)
    for offset in (254, 255):
        data, _, answer = fixture(offsets=[4, 31, offset, 259, 511, 577])
        case(f"cross_chunk_{offset}", data, answer)
    for offset in (29, 30):
        data, first, answer = fixture(offsets=[4, offset, 253, 259, 511, 577])
        case(f"cross_word_{offset}_a2", data, answer)
        case(f"cross_word_{offset}_a1", data, first, active=pack.A2_BASE)
    for sp in (0x24080000, 0x30048000, 0x38010000):
        data, _, answer = fixture(sp=sp)
        case(f"stack_{sp:08x}", data, answer)
    # 最大镜像、整Flash-word、仅16B向量及尾部一过尾地址边界。
    for size in (16, 32, 256, 512, pack.IMAGE_LIMIT):
        data, first, second = fixture(size, [4])
        case(f"image_size_{size}", data, second)

    first, second, version = bytearray(a1), bytearray(a2), bytearray(package[40:116])
    struct.pack_into("<I", first, 577, pack.A1_BASE + len(a1))
    struct.pack_into("<I", second, 577, pack.A2_BASE + len(a2))
    struct.pack_into("<I", version, 16, zlib.crc32(first))
    one_past = pack.build_package(first, version, [4, 31, 253, 259, 511, 577], second)
    case("one_past_pointer", one_past, second)

    for app_build in args.app_build:
        app_build = app_build.resolve()
        output = invoke([sys.executable, str(ROOT / "tools/pack_ota.py"), "--app-build", str(app_build),
                         "--compiler", str(args.compiler)], cwd=ROOT, env=env)
        print(output.strip(), flush=True)
        real = (app_build / "test.ota").read_bytes()
        case(app_build.name + "_a2", real, (app_build / "a2_from_a1.bin").read_bytes())
        case(app_build.name + "_a1", real, (app_build / "test.bin").read_bytes(), active=pack.A2_BASE)
    report = {"scope": "actual C on QEMU Cortex-M7; RAM Flash fault injection; not hardware OTA",
              "configuration": args.configuration, "passed": len(results),
              "packer_negative_checks": 4,
              "core_sha256": hashlib.sha256((ROOT / "boot/ota_install.c").read_bytes()).hexdigest(),
              "test_elf_sha256": hashlib.sha256((build / "ota_core_test.elf").read_bytes()).hexdigest(),
              "cases": results}
    (build / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"PASS {args.configuration}: {len(results)}项实际C离线执行；报告：{build / 'report.json'}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        raise SystemExit(f"FAIL: {error}") from error
