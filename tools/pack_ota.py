#!/usr/bin/env python3
"""把已验证的A1构建打成一个升级包；只生成文件，不访问网络或开发板。

复用App的ELF修正表提取器和既有76B版本载荷，拒绝缺少实验配置的普通bin。
示例：python tools/pack_ota.py --app-build ../test/build/ota-probe-debug-a1
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import zlib


HEADER_BYTES = 40
VERSION_BYTES = 76
PREFIX_BYTES = HEADER_BYTES + VERSION_BYTES
A1_BASE = 0x08020000
A2_BASE = 0x08100000
IMAGE_LIMIT = 896 * 1024
PACKAGE_LIMIT = 2 * 1024 * 1024
MAGIC = 0x3141544F


def load_relocation_tool(path: Path):
    """直接复用已有实现；不能另写一套扫描整数的地址修正逻辑。"""
    spec = importlib.util.spec_from_file_location("app_ota_relocation", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def fixed_text(data: bytes) -> str:
    """与C校验器一致：非空、有终止符，未用字节全部为零。"""
    head, separator, tail = data.partition(b"\0")
    if not head or not separator or any(tail):
        raise ValueError("版本信息字符串未终止或填充非法")
    return head.decode("utf-8")


def build_package(image: bytes, version: bytes, offsets: list[int], a2: bytes) -> bytes:
    """固定小端格式；这里只做封装，调用前须完成ELF提取和独立A2对照。"""
    if len(version) != VERSION_BYTES or not 8 <= len(image) <= IMAGE_LIMIT:
        raise ValueError("版本载荷或镜像长度非法")
    if struct.unpack_from("<HH", version) != (1, VERSION_BYTES) or version[10:12] != b"\0\0":
        raise ValueError("只支持现有76B版本格式，填充字节须为零")
    if struct.unpack_from("<II", version, 12) != (len(image), zlib.crc32(image)):
        raise ValueError("版本载荷不属于这个A1镜像")
    for start, size in ((20, 24), (44, 16), (60, 16)):
        fixed_text(version[start:start + size])
    if len(image) != len(a2) or not offsets or offsets[0] != 4:
        raise ValueError("缺少复位向量修正或A2长度不一致")
    previous = 0
    for offset in offsets:
        if type(offset) is not int or not previous + 4 <= offset <= len(image) - 4:
            raise ValueError("修正偏移须严格递增、不重叠且在镜像内")
        previous = offset
    # 包内不带可执行地址或降级开关，目标槽和版本策略由B的可信请求确定。
    table = struct.pack(f"<{len(offsets)}I", *offsets)
    total = PREFIX_BYTES + len(table) + len(image)
    if total > PACKAGE_LIMIT:
        raise ValueError("升级包超过2MiB下载分区")
    header = struct.pack("<IHHHHIIIIII", MAGIC, 1, HEADER_BYTES, 1, 0, total,
                         len(offsets), zlib.crc32(table), zlib.crc32(a2),
                         zlib.crc32(version), 0)
    assert len(header) == HEADER_BYTES - 4
    return header + struct.pack("<I", zlib.crc32(header)) + version + table + image


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-build", type=Path, required=True)
    parser.add_argument("--app-name", choices=("test", "b1"), default="test")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--relocation-tool", type=Path, default=Path(__file__).resolve().parents[2]
                        / "test/tests/verify_ota_relocation.py")
    parser.add_argument("--compiler", type=Path)
    args = parser.parse_args()
    build = args.app_build.resolve()
    app = args.app_name
    tool = load_relocation_tool(args.relocation_tool)
    compiler = args.compiler
    if compiler is None:
        bundles = Path(os.environ["LOCALAPPDATA"]) / "stm32cube/bundles/gnu-tools-for-stm32"
        compiler = sorted(bundles.glob("*/bin/arm-none-eabi-gcc.exe"))[-1]
    env = dict(os.environ, LC_ALL="C")
    env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
    image = (build / f"{app}.bin").read_bytes()
    version = (build / f"{app}.version.bin").read_bytes()
    elf = build / f"{app}.elf"
    expected_target = "b1-h743" if app == "b1" else "apollo-v2-h743"
    if fixed_text(version[60:76]) != expected_target:
        raise ValueError("App名称与持久化目标板型不一致")
    report = json.loads((build / "relocation_report.json").read_text(encoding="utf-8"))
    # 检查输入bin确实出自当前ELF，防止混用不同构建目录的镜像和修正信息。
    with tempfile.TemporaryDirectory(prefix="ota-pack-", dir=build) as temporary:
        extracted = Path(temporary) / "elf-image.bin"
        tool.run([str(compiler.with_name("arm-none-eabi-objcopy.exe")), "-O", "binary",
                  str(elf), str(extracted)], env=env)
        if extracted.read_bytes() != image:
            raise ValueError("bin与ELF不一致，拒绝发布混合产物")
    sections = tool.load_sections(tool.run(
        [str(compiler.with_name("arm-none-eabi-objdump.exe")), "-h", str(elf)], env=env))
    if sections[".isr_vector"].vma != A1_BASE:
        raise ValueError("发布源必须是A1实验链接，不能拿普通App或A2充当基准")
    readelf = str(compiler.with_name("arm-none-eabi-readelf.exe"))
    symbols = tool.run([readelf, "-Ws", str(elf)], env=env)
    weak = {int(m.group(1)) for m in re.finditer(
        r"^\s*(\d+):\s+0+\s+\d+\s+\S+\s+WEAK\s+DEFAULT\s+UND\s", symbols, re.MULTILINE)}
    offsets, _, _ = tool.extract_offsets(tool.run([readelf, "-Wr", str(elf)], env=env),
                                         sections, image, A1_BASE, weak)
    a2 = tool.rebase(image, offsets, A1_BASE, A2_BASE)
    if not (report["a2_exact_match"] is True and report["reverse_exact_match"] is True and
            report["offsets"] == offsets and report["image_bytes"] == len(image) and
            int(report["a1_crc32"], 16) == zlib.crc32(image) and
            int(report["a2_crc32"], 16) == zlib.crc32(a2) and
            (build / "a2_from_a1.bin").read_bytes() == a2):
        raise ValueError("构建与离线逐字节验证报告不一致，请重新运行App的ota-probe")
    package = build_package(image, version, offsets, a2)
    output = args.output or build / f"{app}.ota"
    inputs = {build / name for name in (f"{app}.bin", f"{app}.elf", f"{app}.version.bin",
              f"{app}.manifest.json", "relocation_report.json", "a2_from_a1.bin")}
    if output.resolve() in inputs:
        raise ValueError("升级包输出不能覆盖输入构建产物")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(package)
    if output.read_bytes() != package:
        raise ValueError("升级包落盘回读失败")
    print(f"PASS: {output}; 一个App={len(image)} B，修正={len(offsets)}项，包={len(package)} B")
    print("只生成文件，不自动发送或烧录；B1尚待真板验收。")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, IndexError) as error:
        raise SystemExit(f"FAIL: {error}") from error
