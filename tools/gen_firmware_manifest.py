#!/usr/bin/env python3
"""生成B1固件清单及与 firmware_version_info_t 一致的76字节载荷。"""

from __future__ import annotations

import argparse
import json
import struct
import zlib
from pathlib import Path


SCHEMA_VERSION = 1
STRUCT_SIZE = 76
# version_patch后补2字节，保持Cortex-M7 C结构中uint32_t字段的4字节对齐。
PAYLOAD_FORMAT = "<HHHHH2xII24s16s16s"


def fixed_text(value: str, size: int, field: str) -> bytes:
    """编码定长 C 字符串，并强制保留结尾 NUL，防止固件侧越界读取。"""

    encoded = value.encode("utf-8")
    if len(encoded) >= size:
        raise ValueError(f"{field} 超过 {size - 1} 字节: {value!r}")
    return encoded + b"\0" * (size - len(encoded))


def parse_semantic_version(value: str) -> tuple[int, int, int]:
    parts = value.split(".")
    if len(parts) != 3:
        raise ValueError("version 必须是 major.minor.patch")
    version = tuple(int(part, 10) for part in parts)
    if any(part < 0 or part > 0xFFFF for part in version):
        raise ValueError("语义版本每一段必须位于 0..65535")
    return version  # type: ignore[return-value]


def build_payload(args: argparse.Namespace, image: bytes) -> tuple[bytes, dict[str, object]]:
    major, minor, patch = parse_semantic_version(args.version)
    image_crc32 = zlib.crc32(image) & 0xFFFFFFFF

    payload = struct.pack(
        PAYLOAD_FORMAT,
        SCHEMA_VERSION,
        STRUCT_SIZE,
        major,
        minor,
        patch,
        len(image),
        image_crc32,
        fixed_text(args.build_time, 24, "build_time"),
        fixed_text(args.build_id, 16, "build_id"),
        fixed_text(args.target, 16, "target"),
    )
    if len(payload) != STRUCT_SIZE:
        raise AssertionError(f"版本载荷大小错误: {len(payload)}")

    manifest: dict[str, object] = {
        "schema_version": SCHEMA_VERSION,
        "semantic_version": args.version,
        "build_time_utc": args.build_time,
        "build_id": args.build_id,
        "target": args.target,
        "image_length": len(image),
        "image_crc32": f"0x{image_crc32:08X}",
        "version_payload_length": len(payload),
    }
    return payload, manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--version-payload", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--build-time", required=True)
    parser.add_argument("--build-id", required=True)
    parser.add_argument("--target", required=True)
    args = parser.parse_args()

    image = args.image.read_bytes()
    if not image:
        parser.error("固件镜像为空")

    payload, manifest = build_payload(args, image)
    args.version_payload.write_bytes(payload)
    args.json.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(
        f"manifest: {manifest['semantic_version']}, {len(image)} bytes, "
        f"CRC32={manifest['image_crc32']}"
    )


if __name__ == "__main__":
    main()
