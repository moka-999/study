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
# configured_gateway后补1字节，与C结构中service_port的uint16_t对齐一致。
# v2在v1站点标签后追加tester_name和test_board，保留旧字段偏移。
FRU_FORMAT = "<HH32s32s16s16s6sB4s4s4sxH16s32s32s22s"
FRU_SIZE = 224


def header_define(name: str, root: Path = ROOT) -> int:
    text = (root / "BSP/Flash/flash_layout.h").read_text(encoding="utf-8")
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


def verify_partition_map(root: Path = ROOT) -> None:
    """核对指定App工程的完整分区，不连接硬件或改写Flash。"""
    total = header_define("FLASH_LAYOUT_TOTAL_SIZE", root)
    metadata_base = header_define("FLASH_LAYOUT_METADATA_BASE", root)
    metadata_size = header_define("FLASH_LAYOUT_METADATA_SIZE", root)
    ota_a = header_define("FLASH_LAYOUT_OTA_SLOT_A_BASE", root)
    ota_b = header_define("FLASH_LAYOUT_OTA_SLOT_B_BASE", root)
    ota_size = header_define("FLASH_LAYOUT_OTA_SLOT_SIZE", root)
    rk_a = header_define("FLASH_LAYOUT_RK_FIRMWARE_SLOT_A_BASE", root)
    rk_b = header_define("FLASH_LAYOUT_RK_FIRMWARE_SLOT_B_BASE", root)
    rk_size = header_define("FLASH_LAYOUT_RK_FIRMWARE_SLOT_SIZE", root)
    reserved = header_define("FLASH_LAYOUT_RESERVED_DATA_BASE", root)
    reserved_size = header_define("FLASH_LAYOUT_RESERVED_DATA_SIZE", root)
    self_test = header_define("FLASH_LAYOUT_SELF_TEST_BASE", root)
    self_test_size = header_define("FLASH_LAYOUT_SELF_TEST_SIZE", root)

    assert total == 32 * 1024 * 1024
    assert metadata_base == 0
    assert metadata_size == 0x10000
    assert (ota_a, ota_b, ota_size) == (0x10000, 0x210000, 0x200000)
    assert (rk_a, rk_b, rk_size) == (0x410000, 0xC10000, 8 * 1024 * 1024)
    assert (self_test, self_test_size) == (0x1FF0000, 0x10000)
    assert metadata_base + metadata_size == ota_a
    assert ota_a + ota_size == ota_b
    assert ota_b + ota_size == rk_a
    assert rk_a + rk_size == rk_b
    assert rk_b + rk_size == reserved
    assert reserved + reserved_size == self_test
    assert self_test + self_test_size == total
    assert reserved_size > 0, "RK预留后仍须保留独立扩展区"

    for name in (
        "FLASH_LAYOUT_VERSION_SLOT_A",
        "FLASH_LAYOUT_VERSION_SLOT_B",
        "FLASH_LAYOUT_FRU_SLOT_A",
        "FLASH_LAYOUT_FRU_SLOT_B",
        "FLASH_LAYOUT_RK_FIRMWARE_SLOT_A_BASE",
        "FLASH_LAYOUT_RK_FIRMWARE_SLOT_B_BASE",
        "FLASH_LAYOUT_RK_FIRMWARE_SLOT_SIZE",
        "FLASH_LAYOUT_RESERVED_DATA_BASE",
        "FLASH_LAYOUT_RESERVED_DATA_SIZE",
        "FLASH_LAYOUT_SELF_TEST_BASE",
    ):
        assert header_define(name, root) % SECTOR_SIZE == 0, f"{name} 未按扇区对齐"


def verify_crc_and_payload_layout() -> None:
    assert zlib.crc32(b"123456789") & 0xFFFFFFFF == 0xCBF43926
    assert HEADER_SIZE == 32
    assert struct.calcsize(PAYLOAD_FORMAT) == STRUCT_SIZE == 76
    assert struct.calcsize(FRU_FORMAT) == FRU_SIZE
    assert header_define("FRU_INFO_SCHEMA_VERSION") == 2


def verify_dual_copy_and_power_loss() -> None:
    record_type = 2
    old_payload = bytes(range(FRU_SIZE))
    new_payload = bytes(reversed(range(FRU_SIZE)))
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
    assert load_latest([slot_a, bytes(corrupt)], record_type, FRU_SIZE).payload == old_payload
    wrapped_old = make_record(record_type, 0xFFFFFFFF, old_payload).ljust(SECTOR_SIZE, b"\xFF")
    wrapped_new = make_record(record_type, 0, new_payload).ljust(SECTOR_SIZE, b"\xFF")
    assert load_latest([wrapped_old, wrapped_new], record_type, FRU_SIZE).payload == new_payload

    # v1的160字节载荷不得被v2当成224字节结构误读。
    old_v1 = make_record(record_type, 9, bytes(range(160))).ljust(SECTOR_SIZE, b"\xFF")
    assert load_latest([old_v1, erased], record_type, FRU_SIZE) is None

    # 迁移首写覆盖较旧v1副本；较新v1在新v2完整前一直保留。
    v1_a = make_record(record_type, 10, bytes(160)).ljust(SECTOR_SIZE, b"\xFF")
    v1_b = make_record(record_type, 11, bytes([1]) * 160).ljust(SECTOR_SIZE, b"\xFF")
    migrated_v2 = make_record(record_type, 12, bytes([2]) * FRU_SIZE).ljust(
        SECTOR_SIZE, b"\xFF")
    assert load_latest([v1_a, v1_b], record_type, 160).generation == 11
    assert load_latest([migrated_v2, v1_b], record_type, FRU_SIZE).generation == 12
    assert load_latest([migrated_v2, v1_b], record_type, 160).generation == 11


def verify_source_contract() -> None:
    layout_c = (ROOT / "BSP/Flash/flash_layout.c").read_text(encoding="utf-8")
    layout_h = (ROOT / "BSP/Flash/flash_layout.h").read_text(encoding="utf-8")
    flash_bsp = (ROOT / "BSP/Flash/flash_bsp.c").read_text(encoding="utf-8")
    qspi = (ROOT / "HAL_Drv/qspi_hw.c").read_text(encoding="utf-8")
    qspi_test = (ROOT / "App/tasks/test/qspi_test.c").read_text(encoding="utf-8")
    tester_info = (ROOT / "App/protocol/pc/pc_tester_info.c").read_text(encoding="utf-8")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    linker = (ROOT / "STM32H743xx_FLASH.ld").read_text(encoding="utf-8")

    assert "generation_is_newer" in layout_c and "payload_crc32" in layout_c
    assert "offsetof(fru_info_t, tester_name) == 138U" in layout_h
    assert "offsetof(fru_info_t, test_board) == 170U" in layout_h
    assert "fru_info_load_legacy_v1" in layout_c
    assert "read_legacy_slot" in layout_c, "选择写入槽前必须纳入旧格式的有效代数"
    assert "record_payload_is_valid" in layout_c, "每槽先验证字段再选择最新记录"
    assert "FLASH_STORE_ERR_UNSUPPORTED" in layout_c and "FLASH_STORE_ERR_LEGACY" in tester_info
    assert "fill_missing_fields" in tester_info, "迁移只补空字段，不覆盖旧板身份"
    for removed_field in ("flags", "build_number", "git_commit", "part_number", "mac_valid"):
        assert removed_field not in layout_h, f"精简布局不应再包含字段: {removed_field}"
    assert "fixed_string_is_terminated" in layout_c, "定长FRU字符串必须校验NUL结尾"
    assert "ipv4_is_zero" in layout_c, "静态模式必须拒绝全零IP和掩码"
    for field in (
        "configured_ipv4",
        "configured_netmask",
        "configured_gateway",
        "service_port",
        "site_label",
        "tester_name",
        "test_board",
    ):
        assert field in layout_c or field in (
            ROOT / "BSP/Flash/flash_layout.h"
        ).read_text(encoding="utf-8"), f"FRU缺少板卡定位字段: {field}"
    assert "memcmp(s_sector_buffer" in flash_bsp, "相同数据应跳过擦写"
    assert "page_is_erased" in flash_bsp, "擦除态页面不应重复编程"
    assert "(addr / W25Q_PAGE_SIZE)" in qspi, "页写必须拒绝跨页"
    assert "SELF_TEST_RAW_ADDRESS" in qspi_test
    assert "0x01FFE000UL" in qspi_test, "真板测试必须覆盖16MiB以上四字节地址"
    assert "SELF_TEST_ALIAS_ADDRESS" in qspi_test, "高地址测试必须检查24位地址回卷"
    assert "firmware_version_save" in qspi_test and "fru_info_save" in qspi_test
    assert 'TESTER_PRODUCT_NAME        "LPDDR4/X_200B"' in tester_info
    assert 'TESTER_NAME                "JMCT-4000"' in tester_info
    assert 'TESTER_HARDWARE_REVISION   "D00"' in tester_info
    assert 'TESTER_BOARD_NAME          "RK3588_test"' in tester_info
    assert "fru_info_load" in tester_info and "fru_info_save" in tester_info
    assert "NVIC_SystemReset" in qspi_test, "真板测试必须验证复位后的数据保持"
    assert "W25Q_CAPACITY - W25Q_SECTOR_SIZE" not in qspi_test
    assert "ENABLE_FLASH_SELF_TEST" in cmake
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
    assert len(payload_path.read_bytes()) == STRUCT_SIZE


def main() -> None:
    verify_partition_map()
    verify_crc_and_payload_layout()
    verify_dual_copy_and_power_loss()
    verify_source_contract()
    verify_current_build_outputs()
    print("flash_layout: 32MiB partition, CRC32, version payload and dual-copy FRU v2 PASS")
    print("power-loss model: every byte interruption point recovered old or complete new record")


if __name__ == "__main__":
    main()
