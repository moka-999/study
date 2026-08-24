#!/usr/bin/env python3
"""离线验证 W25Q256 分区、版本载荷和 FRU 双副本断电恢复。"""

from __future__ import annotations

import json
import re
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from gen_firmware_manifest import PAYLOAD_FORMAT, STRUCT_SIZE  # noqa: E402


MAGIC = 0x43523142
FORMAT_VERSION = 1
SECTOR_SIZE = 0x1000
HEADER_FORMAT = "<IHHIIII8s"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
FRU_FORMAT = "<HH32s32s32s16s16s6sBB4s4s4sH16s22s"


def header_define(name: str) -> int:
    text = (ROOT / "BSP/Flash/flash_layout.h").read_text(encoding="utf-8")
    match = re.search(rf"^#define\s+{name}\s+(0x[0-9A-Fa-f]+|\d+)", text, re.MULTILINE)
    assert match, f"未找到布局常量 {name}"
    return int(match.group(1), 0)


@dataclass(frozen=True)
class Record:
    generation: int
    payload: bytes


def make_record(record_type: int, generation: int, payload: bytes) -> bytes:
    payload_crc = zlib.crc32(payload) & 0xFFFFFFFF
    header_prefix = struct.pack(
        "<IHHIII",
        MAGIC,
        FORMAT_VERSION,
        record_type,
        generation,
        len(payload),
        payload_crc,
    )
    header_crc = zlib.crc32(header_prefix) & 0xFFFFFFFF
    return struct.pack(
        HEADER_FORMAT,
        MAGIC,
        FORMAT_VERSION,
        record_type,
        generation,
        len(payload),
        payload_crc,
        header_crc,
        b"\xFF" * 8,
    ) + payload


def parse_record(slot: bytes, record_type: int, payload_size: int) -> Record | None:
    if len(slot) != SECTOR_SIZE or slot[:4] == b"\xFF" * 4:
        return None
    fields = struct.unpack_from(HEADER_FORMAT, slot)
    magic, version, actual_type, generation, length, payload_crc, header_crc, _ = fields
    if (
        magic != MAGIC
        or version != FORMAT_VERSION
        or actual_type != record_type
        or length != payload_size
        or zlib.crc32(slot[:20]) & 0xFFFFFFFF != header_crc
    ):
        return None
    payload = slot[HEADER_SIZE : HEADER_SIZE + length]
    if zlib.crc32(payload) & 0xFFFFFFFF != payload_crc:
        return None
    return Record(generation, payload)


def generation_is_newer(left: int, right: int) -> bool:
    difference = (left - right) & 0xFFFFFFFF
    return 0 < difference < 0x80000000


def load_latest(slots: list[bytes], record_type: int, payload_size: int) -> Record | None:
    valid = [record for slot in slots if (record := parse_record(slot, record_type, payload_size))]
    if not valid:
        return None
    latest = valid[0]
    for record in valid[1:]:
        if generation_is_newer(record.generation, latest.generation):
            latest = record
    return latest


def verify_partition_map() -> None:
    total = header_define("FLASH_LAYOUT_TOTAL_SIZE")
    metadata_base = header_define("FLASH_LAYOUT_METADATA_BASE")
    metadata_size = header_define("FLASH_LAYOUT_METADATA_SIZE")
    ota_a = header_define("FLASH_LAYOUT_OTA_SLOT_A_BASE")
    ota_b = header_define("FLASH_LAYOUT_OTA_SLOT_B_BASE")
    ota_size = header_define("FLASH_LAYOUT_OTA_SLOT_SIZE")
    reserved = header_define("FLASH_LAYOUT_RESERVED_DATA_BASE")
    reserved_size = header_define("FLASH_LAYOUT_RESERVED_DATA_SIZE")
    self_test = header_define("FLASH_LAYOUT_SELF_TEST_BASE")
    self_test_size = header_define("FLASH_LAYOUT_SELF_TEST_SIZE")

    assert total == 32 * 1024 * 1024
    assert metadata_base == 0
    assert metadata_base + metadata_size == ota_a
    assert ota_a + ota_size == ota_b
    assert ota_b + ota_size == reserved
    assert reserved + reserved_size == self_test
    assert self_test + self_test_size == total
    assert ota_size >= 1920 * 1024, "单个 OTA 预留槽必须容纳当前最大 App"

    for name in (
        "FLASH_LAYOUT_VERSION_SLOT_A",
        "FLASH_LAYOUT_VERSION_SLOT_B",
        "FLASH_LAYOUT_FRU_SLOT_A",
        "FLASH_LAYOUT_FRU_SLOT_B",
        "FLASH_LAYOUT_SELF_TEST_BASE",
    ):
        assert header_define(name) % SECTOR_SIZE == 0, f"{name} 未按扇区对齐"


def verify_crc_and_payload_layout() -> None:
    assert zlib.crc32(b"123456789") & 0xFFFFFFFF == 0xCBF43926
    assert HEADER_SIZE == 32
    assert struct.calcsize(PAYLOAD_FORMAT) == STRUCT_SIZE == 96
    assert struct.calcsize(FRU_FORMAT) == 192


def verify_dual_copy_and_power_loss() -> None:
    record_type = 2
    old_payload = bytes(range(192))
    new_payload = bytes(reversed(range(192)))
    old_record = make_record(record_type, 7, old_payload)
    new_record = make_record(record_type, 8, new_payload)

    slot_a = old_record.ljust(SECTOR_SIZE, b"\xFF")
    erased = b"\xFF" * SECTOR_SIZE

    # 模拟更新备用扇区时在任意字节后掉电：新记录未完整前必须回退到旧副本。
    for written in range(len(new_record) + 1):
        interrupted = (new_record[:written] + erased[written:])[:SECTOR_SIZE]
        latest = load_latest([slot_a, interrupted], record_type, len(old_payload))
        assert latest is not None
        expected = new_payload if written == len(new_record) else old_payload
        assert latest.payload == expected, f"掉电点 {written} 未恢复到完整记录"

    # 最新副本损坏时回退旧副本；generation 回绕时 0 比 0xFFFFFFFF 新。
    corrupt = bytearray(new_record.ljust(SECTOR_SIZE, b"\xFF"))
    corrupt[HEADER_SIZE + 3] ^= 0x01
    assert load_latest([slot_a, bytes(corrupt)], record_type, 192).payload == old_payload
    wrapped_old = make_record(record_type, 0xFFFFFFFF, old_payload).ljust(SECTOR_SIZE, b"\xFF")
    wrapped_new = make_record(record_type, 0, new_payload).ljust(SECTOR_SIZE, b"\xFF")
    assert load_latest([wrapped_old, wrapped_new], record_type, 192).payload == new_payload


def verify_source_contract() -> None:
    layout_c = (ROOT / "BSP/Flash/flash_layout.c").read_text(encoding="utf-8")
    flash_bsp = (ROOT / "BSP/Flash/flash_bsp.c").read_text(encoding="utf-8")
    qspi = (ROOT / "HAL_Drv/qspi_hw.c").read_text(encoding="utf-8")
    qspi_test = (ROOT / "App/tasks/qspi_test.c").read_text(encoding="utf-8")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    linker = (ROOT / "STM32H743xx_FLASH.ld").read_text(encoding="utf-8")

    assert "generation_is_newer" in layout_c and "payload_crc32" in layout_c
    assert "fixed_string_is_terminated" in layout_c, "定长FRU字符串必须校验NUL结尾"
    assert "ipv4_is_zero" in layout_c, "静态模式必须拒绝全零IP和掩码"
    for field in (
        "configured_ipv4",
        "configured_netmask",
        "configured_gateway",
        "service_port",
        "site_label",
    ):
        assert field in layout_c or field in (
            ROOT / "BSP/Flash/flash_layout.h"
        ).read_text(encoding="utf-8"), f"FRU缺少板卡定位字段: {field}"
    assert "memcmp(s_sector_buffer" in flash_bsp, "相同数据应跳过擦写"
    assert "page_is_erased" in flash_bsp, "擦除态页面不应重复编程"
    assert "(addr / W25Q_PAGE_SIZE)" in qspi, "页写必须拒绝跨页"
    assert "FLASH_LAYOUT_SELF_TEST_BASE" in qspi_test
    assert "W25Q_CAPACITY - W25Q_SECTOR_SIZE" not in qspi_test
    assert "gen_firmware_manifest.py" in cmake
    assert "KEEP(*(.firmware_version))" in linker, "编译期版本信息被链接器回收"


def verify_current_build_outputs() -> None:
    build_dir = ROOT / "build_debug"
    manifest_path = build_dir / "test.manifest.json"
    payload_path = build_dir / "test.version.bin"
    image_path = build_dir / "test.bin"
    if not (manifest_path.exists() and payload_path.exists() and image_path.exists()):
        return

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    image = image_path.read_bytes()
    assert manifest["image_length"] == len(image)
    assert manifest["image_crc32"] == f"0x{zlib.crc32(image) & 0xFFFFFFFF:08X}"
    assert len(payload_path.read_bytes()) == 96


def main() -> None:
    verify_partition_map()
    verify_crc_and_payload_layout()
    verify_dual_copy_and_power_loss()
    verify_source_contract()
    verify_current_build_outputs()
    print("flash_layout: 32MiB partition, CRC32, version payload and dual-copy FRU PASS")
    print("power-loss model: every byte interruption point recovered old or complete new record")


if __name__ == "__main__":
    main()
