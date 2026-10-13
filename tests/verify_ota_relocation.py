#!/usr/bin/env python3
"""用真实App验证“一个镜像 + 地址修正表”，不访问开发板、不生成正式升级包。

入口：powershell -ExecutionPolicy Bypass -File tools/dev.ps1 ota-probe
先用同一源码/编译配置生成A1，再独立链接A2作为答案；只从A1的ELF提取
地址修正位置，对A1裸镜像执行修正，最后与A2逐字节比较。独立链接A2只用于
验证；实板应烧录导出的a2_from_a1.bin，而不是参考A2。脚本自身不访问烧录器，
全部实验产物留在忽略提交的build目录，不是正式OTA包。
"""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import zlib


A1_BASE = 0x08020000
A2_BASE = 0x08100000
IMAGE_LIMIT = 896 * 1024


@dataclass(frozen=True)
class LoadSection:
    """只记录实际写入Flash的节；.data的运行地址在RAM，装载地址在Flash。"""

    size: int
    vma: int
    lma: int


def run(command: list[str], *, env: dict[str, str], log: Path | None = None) -> str:
    result = subprocess.run(command, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", env=env)
    output = result.stdout + result.stderr
    if log is not None:
        log.write_text(output, encoding="utf-8")
    if result.returncode:
        raise ValueError(f"命令失败({result.returncode}): {command[0]}\n{output[-6000:]}")
    return output


def load_sections(output: str) -> dict[str, LoadSection]:
    """读取本机GNU objdump -h的节表；调试段和NOLOAD段不进入裸镜像。"""
    sections = {}
    lines = output.splitlines()
    for index, line in enumerate(lines[:-1]):
        fields = re.match(r"\s*\d+\s+(\S+)\s+([\da-fA-F]+)\s+"
                          r"([\da-fA-F]+)\s+([\da-fA-F]+)\s+", line)
        if fields is None:
            continue
        flags = {flag.strip() for flag in lines[index + 1].split(",")}
        if not {"CONTENTS", "ALLOC", "LOAD"} <= flags:
            continue
        name, size, vma, lma = fields.groups()
        section = LoadSection(*(int(value, 16) for value in (size, vma, lma)))
        if section.size:
            sections[name] = section
    if ".isr_vector" not in sections or ".text" not in sections:
        raise ValueError("没有找到App装载节，拒绝对未知ELF布局做猜测")
    return sections


def relocation_action(kind: str, symbol_moves: bool, place_moves: bool,
                      undefined_weak: bool = False) -> bool:
    """True仅表示对该32位绝对地址加槽间偏移；相对地址必须整体同移。

    ponytail: 只接纳本GNU工具链已观察到的5种类型；新增类型立即失败，
    待按ARM ABI补充解码及独立链接对照后再扩展，绝不扫描“像地址”的整数。
    """
    if kind == "R_ARM_NONE":
        return False
    if kind == "R_ARM_ABS32":
        return symbol_moves
    if kind in {"R_ARM_PREL31", "R_ARM_THM_CALL", "R_ARM_THM_JUMP24"}:
        # ARM ABI 5.5.1.1：未定义弱符号的PC相对引用由静态链接器按place
        # 处理。它不是到0地址的跨区调用；须由符号表确认WEAK+UND，不能
        # 仅凭符号值为0放行。这类已链接指令随位置整体移动即可。
        if undefined_weak:
            return False
        if symbol_moves != place_moves:
            raise ValueError(f"{kind}: 跨固定/移动区域的相对引用尚未支持")
        return False
    raise ValueError(f"不支持的重定位类型: {kind}")


def extract_offsets(output: str, sections: dict[str, LoadSection], image: bytes,
                    base: int, weak_indices: set[int]) -> tuple[list[int], Counter, Counter]:
    """只依赖源ELF生成修正表，不读取A2参考字节。

    重定位位置使用节的VMA定位，再转换为Flash LMA偏移；不能把.data内的
    函数指针误当成“RAM无需处理”。符号和最终地址都必须落在源镜像范围。
    """
    offsets: set[int] = set()
    kinds: Counter = Counter()
    patched_sections: Counter = Counter()
    owner = ""
    expected = seen = 0
    vector_symbol_seen = False

    def finish_section() -> None:
        if owner in sections and seen != expected:
            raise ValueError(f"{owner}: 重定位表读取不完整({seen}/{expected})")

    for line in output.splitlines():
        heading = re.match(r"Relocation section '([^']+)'.*contains (\d+) entr(?:y|ies)", line)
        if heading:
            finish_section()
            name, count = heading.groups()
            if name.startswith(".rela") and name[5:] in sections:
                raise ValueError("尚不支持RELA格式的装载节")
            owner, expected, seen = name[4:], int(count), 0
            continue
        if owner not in sections:
            continue
        entry = re.match(r"\s*([\da-fA-F]+)\s+([\da-fA-F]+)\s+(R_ARM_\S+)\s+"
                         r"([\da-fA-F]+)\s*(.*)", line)
        if entry is None:
            continue
        place_text, info_text, kind, symbol_text, symbol_name = entry.groups()
        place, symbol = int(place_text, 16), int(symbol_text, 16)
        section = sections[owner]
        seen += 1
        kinds[kind] += 1
        if kind == "R_ARM_NONE":
            continue
        relative = place - section.vma
        offset = section.lma - base + relative
        if not (0 <= relative <= section.size - 4 and 0 <= offset <= len(image) - 4):
            raise ValueError(f"{owner}: 重定位位置越界: 0x{place:08X}")
        # 允许指向镜像结尾的一过尾符号；RAM及外设地址不跟着App搬移。
        symbol_moves = base <= symbol <= base + len(image)
        place_moves = base <= section.vma < base + len(image)
        undefined_weak = (int(info_text, 16) >> 8) in weak_indices
        if not relocation_action(kind, symbol_moves, place_moves, undefined_weak):
            continue
        value = struct.unpack_from("<I", image, offset)[0]
        if not base <= value <= base + len(image):
            raise ValueError(f"{symbol_name}: ABS32最终值不在镜像内: 0x{value:08X}")
        if offset in offsets:
            raise ValueError(f"重复的地址修正: 0x{offset:X}")
        offsets.add(offset)
        patched_sections[owner] += 1
        vector_symbol_seen |= symbol_name.strip() == "g_pfnVectors"
    finish_section()
    if not offsets or 4 not in offsets or not vector_symbol_seen:
        raise ValueError("缺少复位向量或App内g_pfnVectors引用；检查--emit-relocs和VTOR入口")
    ordered = sorted(offsets)
    if any(right - left < 4 for left, right in zip(ordered, ordered[1:])):
        raise ValueError("地址修正位置存在字节重叠")
    return ordered, kinds, patched_sections


def rebase(image: bytes, offsets: list[int], source_base: int, target_base: int) -> bytes:
    """模拟B对唯一源镜像的地址修正；本轮只在PC内存中操作，不擦写Flash。"""
    if {source_base, target_base} != {A1_BASE, A2_BASE}:
        raise ValueError("目标只能是项目规定的两个App槽，禁止任意内部地址")
    if not 8 <= len(image) <= IMAGE_LIMIT:
        raise ValueError("镜像长度超出两槽共同容量")
    result = bytearray(image)
    previous = -4
    for offset in offsets:
        if offset < previous + 4 or offset < 0 or offset > len(image) - 4:
            raise ValueError("修正表必须升序、不重叠且不越界")
        value = struct.unpack_from("<I", image, offset)[0]
        if not source_base <= value <= source_base + len(image):
            raise ValueError("待修正值不属于源镜像，拒绝改动RAM/外设/任意数据")
        struct.pack_into("<I", result, offset, value + target_base - source_base)
        previous = offset
    return bytes(result)


def self_check() -> None:
    """最小回归：Thumb位保留、往返、坏表/未知类型/跨区相对引用拒绝。"""
    source = struct.pack("<IIII", 0x20020000, A1_BASE + 9, A1_BASE + 12, 0x12345678)
    moved = rebase(source, [4, 8], A1_BASE, A2_BASE)
    assert struct.unpack("<IIII", moved) == (0x20020000, A2_BASE + 9, A2_BASE + 12, 0x12345678)
    assert rebase(moved, [4, 8], A2_BASE, A1_BASE) == source
    bad_calls = [
        lambda: rebase(source, [0], A1_BASE, A2_BASE),
        lambda: rebase(source, [4, 4], A1_BASE, A2_BASE),
        lambda: rebase(source, [8, 4], A1_BASE, A2_BASE),
        lambda: rebase(source, [13], A1_BASE, A2_BASE),
        lambda: rebase(source, [4], A1_BASE, 0x08000000),
        lambda: rebase(b"", [], A1_BASE, A2_BASE),
        lambda: relocation_action("R_ARM_UNKNOWN", False, False),
        lambda: relocation_action("R_ARM_THM_CALL", False, True),
    ]
    for call in bad_calls:
        try:
            call()
        except ValueError:
            continue
        raise AssertionError("非法输入没有被拒绝")
    assert not relocation_action("R_ARM_PREL31", True, True)
    assert not relocation_action("R_ARM_ABS32", False, True)
    assert not relocation_action("R_ARM_THM_CALL", False, True, undefined_weak=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--ninja", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--app-root", type=Path, default=Path(__file__).resolve().parents[1],
                        help="待验证App工程；B1复用本工具，不复制V2硬件测试")
    parser.add_argument("--app-name", choices=("test", "b1"), default="test")
    parser.add_argument("--rk-uart", choices=(2, 4), type=int, default=4, help="仅B1构建使用")
    parser.add_argument("--build-type", choices=("Debug", "Release"), default="Debug")
    parser.add_argument("--ota-boot", action="store_true", help="启用真实B日志/健康确认，仍不烧录")
    parser.add_argument("--version", default="0.1.0", help="打包语义版本，例如0.1.1")
    parser.add_argument("--rk-gateway-test", action="store_true", help="仅V2临时全链路联调镜像，正式业务不得启用")
    parser.add_argument("--power-simulation", action="store_true", help="V2正式任务联调：仅模拟电源/测量，RK仍为真实串口")
    parser.add_argument("--mats-fail-test", action="store_true", help="仅V2电源模拟：构建每次MATS结果报FAIL的联调镜像")
    args = parser.parse_args()
    self_check()
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version) or any(int(v) > 65535 for v in args.version.split(".")):
        raise ValueError("版本必须是三个0..65535整数")
    root = args.app_root.resolve()
    app = args.app_name
    if args.rk_gateway_test and app != "test":
        raise ValueError("B1禁止使用V2临时业务门禁模拟")
    if args.power_simulation and (app != "test" or args.rk_gateway_test):
        raise ValueError("电源模拟仅用于V2真实RK路径，不能与旧网关联调开关混用")
    if args.mats_fail_test and (app != "test" or not args.power_simulation or root.name == "b1"):
        raise ValueError("MATS失败联调仅用于V2电源模拟镜像，禁止B1启用")
    env = dict(os.environ)
    env["PATH"] = str(args.compiler.parent) + os.pathsep + env.get("PATH", "")
    # 两槽必须共用构建时间；日常入口传入本轮时间，独立实验保留固定默认值。
    env.setdefault("SOURCE_DATE_EPOCH", "1789430400")
    env["LC_ALL"] = "C"
    builds = []
    for name, base in (("a1", A1_BASE), ("a2", A2_BASE)):
        prefix = "ota-app" if args.ota_boot else "ota-probe"
        if args.rk_gateway_test:
            prefix += "-rk-test"
        if args.power_simulation:
            prefix += "-power-sim"
        if args.mats_fail_test:
            prefix += "-mats-fail"
        suffix = "-uart2" if app == "b1" and args.rk_uart == 2 else ""
        build = root / "build" / f"{prefix}-{args.version}-{args.build_type.lower()}-{name}{suffix}"
        build.mkdir(parents=True, exist_ok=True)
        configure = [str(args.cmake), "-S", str(root), "-B", str(build), "-G", "Ninja",
                     f"-DCMAKE_BUILD_TYPE={args.build_type}",
                     f"-DCMAKE_TOOLCHAIN_FILE={root / 'cmake/gcc-arm-none-eabi.cmake'}",
                     f"-DCMAKE_MAKE_PROGRAM={args.ninja}",
                     f"-DPython3_EXECUTABLE={sys.executable}",
                     "-DENABLE_OTA_RELOCATION_PROBE=ON", f"-DOTA_APP_BASE=0x{base:08X}"]
        configure += [f"-DENABLE_OTA_BOOT={'ON' if args.ota_boot else 'OFF'}"]
        if app == "test":
            configure += [f"-DENABLE_RK_GATEWAY_TEST={'ON' if args.rk_gateway_test else 'OFF'}"]
            configure += [f"-DENABLE_V2_POWER_SIMULATION={'ON' if args.power_simulation else 'OFF'}"]
            configure += [f"-DENABLE_V2_MATS_FAIL_TEST={'ON' if args.mats_fail_test else 'OFF'}"]
        if app == "b1":
            configure += [f"-DB1_RK_UART={args.rk_uart}"]
        configure += [f"-DFIRMWARE_VERSION_{part}={value}" for part, value in
                      zip(("MAJOR", "MINOR", "PATCH"), args.version.split("."))]
        run(configure, env=env, log=build / "configure.log")
        # 实验目录只生成App；A2固定跳转B由独立Boot工程构建，避免混用旧A1入口。
        run([str(args.cmake), "--build", str(build), "--target", app, "--parallel", "8"],
            env=env, log=build / "build.log")
        builds.append(build)

    objdump = args.compiler.with_name("arm-none-eabi-objdump.exe")
    readelf = args.compiler.with_name("arm-none-eabi-readelf.exe")
    sections = [load_sections(run([str(objdump), "-h", str(b / f"{app}.elf")], env=env))
                for b in builds]
    images = [(build / f"{app}.bin").read_bytes() for build in builds]
    if len(images[0]) != len(images[1]) or sections[0].keys() != sections[1].keys():
        raise ValueError("两次链接的镜像大小/装载节发生变化，不能用固定偏移转换")
    delta = A2_BASE - A1_BASE
    for name, left in sections[0].items():
        right = sections[1][name]
        vma_delta = delta if A1_BASE <= left.vma < A1_BASE + len(images[0]) else 0
        if right != LoadSection(left.size, left.vma + vma_delta, left.lma + delta):
            raise ValueError(f"{name}: 链接布局不是整体平移")
    if sections[0][".isr_vector"].vma != A1_BASE or sections[1][".isr_vector"].vma != A2_BASE:
        raise ValueError("链接地址不是规定的A1/A2地址")
    relocations = run([str(readelf), "-Wr", str(builds[0] / f"{app}.elf")], env=env)
    symbols = run([str(readelf), "-Ws", str(builds[0] / f"{app}.elf")], env=env)
    weak_indices = {int(match.group(1)) for match in re.finditer(
        r"^\s*(\d+):\s+0+\s+\d+\s+\S+\s+WEAK\s+DEFAULT\s+UND\s", symbols, re.MULTILINE)}
    offsets, kinds, patched_sections = extract_offsets(
        relocations, sections[0], images[0], A1_BASE, weak_indices)
    converted = rebase(images[0], offsets, A1_BASE, A2_BASE)
    if converted != images[1]:
        mismatch = [i for i, (a, b) in enumerate(zip(converted, images[1])) if a != b]
        raise ValueError(f"A2逐字节对照失败: {len(mismatch)}字节不同，首个偏移0x{mismatch[0]:X}")
    if rebase(converted, offsets, A2_BASE, A1_BASE) != images[0]:
        raise ValueError("A2反向还原A1失败")
    # 仅在两项精确比对通过后导出，保证实板用的是源A1修正产物。
    converted_path = builds[0] / "a2_from_a1.bin"
    converted_path.write_bytes(converted)
    if converted_path.read_bytes() != images[1]:
        raise ValueError("实板候选镜像落盘校验失败")
    report = {
        "app_name": app,
        "scope": "offline-only; no boot installer, no board execution, not an OTA package",
        "build_type": args.build_type,
        "v2_mats_fail_test": args.mats_fail_test,
        "image_bytes": len(images[0]), "relocation_count": len(offsets),
        "offset_table_bytes": 4 * len(offsets),
        "arm_relocation_types": dict(kinds), "patched_sections": dict(patched_sections),
        "a1_crc32": f"0x{zlib.crc32(images[0]):08X}",
        "a2_crc32": f"0x{zlib.crc32(converted):08X}",
        "a2_exact_match": True, "reverse_exact_match": True,
        "offsets": offsets,
    }
    destination = builds[0] / "relocation_report.json"
    destination.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"PASS {args.build_type}: {len(images[0])}字节，{len(offsets)}处地址修正；"
          "A2逐字节一致，反向A1一致；不代表真板通过。")
    print(f"报告: {destination}")
    print(f"A2实板候选（由A1修正）: {converted_path}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit(f"FAIL: {error}") from error
