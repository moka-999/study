#!/usr/bin/env python3
"""生成实板验证输入，不访问板卡。只处理本仓库已逐字节核验并打包的OTA产物。

--journal省略时生成A1首次部署记录；提供时从NOR读取快照生成普通升级请求。
输出必须再交给要求完整备份的board_ota.py，不能把RAM结构直接烧到日志扇区。
"""
import argparse
import json
from pathlib import Path
import struct
import zlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-build", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--journal", type=Path)
    args = parser.parse_args()
    if not args.name or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789_-" for c in args.name):
        raise ValueError("name只允许小写字母数字横线下划线")
    package = (args.app_build / "test.ota").read_bytes()
    image = (args.app_build / "test.bin").read_bytes()
    report = json.loads((args.app_build / "relocation_report.json").read_text(encoding="utf-8"))
    if not report["a2_exact_match"] or not report["reverse_exact_match"]:
        raise ValueError("缺少A1/A2逐字节核验")
    if (len(package) < 116 or package[:4] != b"OTA1" or
        struct.unpack_from("<I", package, 12)[0] != len(package) or
        struct.unpack_from("<I", package, 36)[0] != zlib.crc32(package[:36]) or
        struct.unpack_from("<I", package, 28)[0] != zlib.crc32(package[40:116]) or
        struct.unpack_from("<I", package, 52)[0] != len(image) or
        struct.unpack_from("<I", package, 56)[0] != zlib.crc32(image) or
        not package.endswith(image) or package[100:116] != b"apollo-v2-h743".ljust(16, b"\0")):
        raise ValueError("包/元数据/实际镜像不一致")
    package_crc = struct.unpack_from("<I", package, 36)[0]
    if args.journal:
        snapshot = args.journal.read_bytes()
        if len(snapshot) < 0x6000:
            raise ValueError("journal输入须为NOR从0开始、至少0x6000字节的快照")
        records = []
        for offset in range(0x4000, 0x6000, 256):
            page = snapshot[offset:offset + 256]
            if (page[:8] == b"BST1\x01\x00\x00\x01" and
                struct.unpack_from("<I", page, 252)[0] == 0xC001CAFE and
                struct.unpack_from("<I", page, 248)[0] == zlib.crc32(page[:248])):
                records.append(bytearray(page[8:60] + page[64:248]))
        if not records:
            raise ValueError("没有完整日志记录")
        record = records[0]
        for candidate in records[1:]:
            delta = (struct.unpack_from("<I", candidate)[0] - struct.unpack_from("<I", record)[0]) & 0xFFFFFFFF
            if delta == 0 or delta == 0x80000000:
                raise ValueError("启动代数有歧义")
            if delta < 0x80000000:
                record = candidate
        fields = list(struct.unpack_from("<13I", record))
        if fields[1] != 0 or fields[5] != 0:
            raise ValueError("仅可向已确认NORMAL、pending=0的App提交请求")
        fields[1] = 1; fields[8] = 0; fields[9] = len(package); fields[10] = package_crc
        struct.pack_into("<13I", record, 0, *fields)
    else:
        descriptor = struct.pack("<4I", 0x08020000, len(image), zlib.crc32(image), package_crc) + package[40:116]
        record = struct.pack("<13I", 0, 0, 0, 0xFFFFFFFF, 1, 0, 0, 0, 0, 0, 0, 0, 0) + descriptor + bytes(92)
    assert len(record) == 236
    outputs = {"record.bin": record, "package-4k.bin": package + b"\xff" * (-len(package) % 4096)}
    for suffix in outputs:
        if (args.directory / f"{args.name}-{suffix}").exists():
            raise ValueError("输出已存在，禁止覆盖")
    for suffix, data in outputs.items():
        destination = args.directory / f"{args.name}-{suffix}"
        destination.write_bytes(data)
        print(f"生成 {destination}: {len(data)}字节，未访问板卡")


if __name__ == "__main__":
    main()
