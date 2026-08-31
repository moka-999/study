#!/usr/bin/env python3
"""B1版本清单的纯内存回归；不写Flash，不依赖真板或额外Python库。"""

from pathlib import Path
import struct
import sys
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from gen_firmware_manifest import build_payload, fixed_text, parse_semantic_version


def main() -> None:
    args = SimpleNamespace(
        version="1.2.3", build_time="2026-08-31T00:00:00Z",
        build_id="unit-test", target="b1-h743",
    )
    # CRC32公开标准校验串，避免仅用同一实现计算期望值而掩盖算法错误。
    payload, manifest = build_payload(args, b"123456789")
    expected = struct.pack(
        "<HHHHH2xII24s16s16s", 1, 76, 1, 2, 3, 9, 0xCBF43926,
        b"2026-08-31T00:00:00Z", b"unit-test", b"b1-h743",
    )
    assert payload == expected and len(payload) == 76
    assert payload[10:12] == b"\0\0", "uint32字段前必须保留2字节对齐填充"
    assert manifest == {
        "schema_version": 1, "semantic_version": "1.2.3",
        "build_time_utc": args.build_time, "build_id": "unit-test",
        "target": "b1-h743", "image_length": 9,
        "image_crc32": "0xCBF43926", "version_payload_length": 76,
    }, "清单不得再带build_number/git_commit/git_dirty旧字段"
    assert parse_semantic_version("0.65535.0") == (0, 65535, 0)
    for version in ("1.2", "1.2.3.4", "-1.0.0", "65536.0.0", "a.0.0"):
        try:
            parse_semantic_version(version)
        except ValueError:
            pass
        else:
            raise AssertionError(f"应拒绝非法版本: {version}")
    assert fixed_text("a" * 15, 16, "build_id") == b"a" * 15 + b"\0"
    for value in ("a" * 16, "板" * 6):
        try:
            fixed_text(value, 16, "target")
        except ValueError:
            pass
        else:
            raise AssertionError("定长字符串必须按UTF-8字节数保留NUL")
    print("B1 76-byte manifest layout, CRC32, target and input bounds PASS")


if __name__ == "__main__":
    main()
