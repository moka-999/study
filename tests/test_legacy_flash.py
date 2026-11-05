#!/usr/bin/env python3
"""B1旧Flash布局离线合同：解析实际C声明验ABI，模型验字段/双副本保护。

只使用标准库，不接板、不写Flash。此测试不是执行C驱动；须与ARM构建及
后续真板掉电测试一起使用，不能把模型通过写成硬件验收通过。
"""
from __future__ import annotations

import ctypes as ct
from pathlib import Path
import re
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "BSP/Flash/flash_layout.h").read_text(encoding="utf-8-sig")
SOURCE = (ROOT / "BSP/Flash/flash_layout.c").read_text(encoding="utf-8-sig")
QUERY = (ROOT / "App/protocol/pc/pc_tester_info.c").read_text(encoding="utf-8-sig")
BOOT = (ROOT / "boot/boot_fru.c").read_text(encoding="utf-8-sig")


def c_struct(name: str, text: str) -> type[ct.Structure]:
    """从实际C字段生成同宽小端结构，避免测试手抄一套旧偏移。"""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    match = re.search(r"typedef\s+struct\s*\{([^{}]*)\}\s*" + name + r"\s*;", text)
    assert match, name
    scalars = {"uint8_t": ct.c_uint8, "uint16_t": ct.c_uint16,
               "uint32_t": ct.c_uint32, "char": ct.c_char}
    fields = []
    for declaration in match[1].split(";"):
        if not declaration.strip():
            continue
        field = re.fullmatch(r"\s*(\w+)\s+(\w+)(?:\[(\d+)\])?\s*", declaration)
        assert field and field[1] in scalars, declaration
        kind = scalars[field[1]]
        fields.append((field[2], kind * int(field[3]) if field[3] else kind))
    return type(name, (ct.LittleEndianStructure,), {"_fields_": fields})


FRU = c_struct("fru_info_t", HEADER)
FRU160 = c_struct("fru_info_v1_t", SOURCE)
FRU192 = c_struct("fru_info_b1_v1_t", SOURCE)
VERSION = c_struct("firmware_version_info_t", HEADER)
VERSION96 = c_struct("firmware_version_legacy_t", SOURCE)
RECORD_HEADER = c_struct("flash_record_header_t", HEADER)
LAYOUTS = {(2, 2, 224): FRU, (2, 1, 160): FRU160, (2, 1, 192): FRU192,
           (1, 1, 76): VERSION, (1, 1, 96): VERSION96}


def copy_common(source: ct.Structure, destination: ct.Structure) -> None:
    names = {name for name, _ in source._fields_}
    for name, kind in destination._fields_:
        if name not in names or name in {"schema_version", "struct_size"}:
            continue
        value = getattr(source, name)
        if issubclass(kind, ct.Array) and kind._type_ is not ct.c_char:
            getattr(destination, name)[:] = value[:]
        else:
            setattr(destination, name, value)


def convert_fru(old: ct.Structure) -> ct.Structure:
    new = FRU()
    new.schema_version, new.struct_size = 2, ct.sizeof(FRU)
    copy_common(old, new)
    if hasattr(old, "mac_valid") and not old.mac_valid:
        new.mac_address[:] = bytes(6)
    new.product_name = new.product_name or b"LPDDR4/X_200B"
    new.tester_name = new.tester_name or new.product_name
    new.hardware_revision = new.hardware_revision or b"D00"
    new.test_board = new.test_board or b"RK3588_test"
    return new


def slot(payload: ct.Structure | bytes, generation: int, kind: int = 2,
         format_version: int = 1) -> bytes:
    data = bytes(payload)
    prefix = struct.pack("<IHHIII", 0x43523142, format_version, kind,
                         generation, len(data), zlib.crc32(data))
    record = prefix + struct.pack("<I", zlib.crc32(prefix)) + b"\xff" * 8 + data
    return record.ljust(4096, b"\xff")


def probe(data: bytes, kind: int = 2) -> tuple[str, int]:
    header = RECORD_HEADER.from_buffer_copy(data[:32])
    if header.magic == 0xFFFFFFFF:
        return "empty", 0
    if header.magic != 0x43523142 or header.header_crc32 != zlib.crc32(data[:20]):
        return "invalid", 0
    if header.payload_length > 4064:
        return "unsupported", header.generation
    payload = data[32:32 + header.payload_length]
    if header.payload_crc32 != zlib.crc32(payload):
        return "invalid", 0
    prefix = struct.unpack("<HH", payload[:4]) if len(payload) >= 4 else (0, 0)
    known = (header.format_version == 1 and header.record_type == kind and
             prefix[1] == len(payload) and (kind, prefix[0], len(payload)) in LAYOUTS)
    if not known:
        return "unsupported", header.generation
    layout = LAYOUTS[(kind, prefix[0], len(payload))]
    value = layout.from_buffer_copy(payload)
    # 不能只看CRC：已知布局的每个定长字符串都必须在容量内终止。
    for name, field_type in layout._fields_:
        if issubclass(field_type, ct.Array) and field_type._type_ is ct.c_char:
            offset = getattr(layout, name).offset
            if b"\0" not in payload[offset:offset + ct.sizeof(field_type)]:
                return "invalid", 0
    if kind == 2:
        if value.network_mode > 2 or (hasattr(value, "mac_valid") and value.mac_valid > 1):
            return "invalid", 0
        if value.network_mode != 0 and value.service_port == 0:
            return "invalid", 0
        if value.network_mode == 1 and (not any(value.configured_ipv4) or not any(value.configured_netmask)):
            return "invalid", 0
    return "valid", header.generation


def newer(left: int, right: int) -> bool:
    return ct.c_int32((left - right) & 0xFFFFFFFF).value > 0


def read_choice(a: bytes, b: bytes, kind: int = 2) -> tuple[str, int, int]:
    states = [probe(a, kind), probe(b, kind)]
    if any(state == "unsupported" for state, _ in states):
        raise ValueError("unknown valid record forbids fallback")
    valid = [index for index, (state, _) in enumerate(states) if state == "valid"]
    if not valid:
        return "missing", -1, 0
    current_size = 224 if kind == 2 else 76
    latest = valid[0]
    for candidate in valid[1:]:
        size = RECORD_HEADER.from_buffer_copy((a, b)[candidate][:32]).payload_length
        if newer(states[candidate][1], states[latest][1]) or (
                states[candidate][1] == states[latest][1] and size == current_size):
            latest = candidate
    size = RECORD_HEADER.from_buffer_copy((a, b)[latest][:32]).payload_length
    action = "legacy" if kind == 2 and size != current_size else "current"
    return action, latest, states[latest][1]


def write_target(a: bytes, b: bytes, kind: int = 2) -> tuple[int, int]:
    """对应保存前的共同规则：旧/新已知布局一起比较，未知布局一票否决。"""
    first, second = probe(a, kind), probe(b, kind)
    if "unsupported" in (first[0], second[0]):
        raise ValueError("unknown record must not be overwritten")
    if first[0] == second[0] == "valid":
        latest = 1 if newer(second[1], first[1]) else 0
        generation = (first, second)[latest][1]
        return 1 - latest, (generation + 1) & 0xFFFFFFFF
    if first[0] == "valid":
        return 1, (first[1] + 1) & 0xFFFFFFFF
    if second[0] == "valid":
        return 0, (second[1] + 1) & 0xFFFFFFFF
    return 0, 1


def main() -> None:
    for cls, length in ((FRU, 224), (FRU160, 160), (FRU192, 192),
                        (VERSION, 76), (VERSION96, 96), (RECORD_HEADER, 32)):
        assert ct.sizeof(cls) == length, cls.__name__
    assert FRU.tester_name.offset == 138 and FRU.test_board.offset == 170
    assert FRU.reserved.offset == 202 and FRU192.serial_number.offset == 68
    assert VERSION.image_length.offset == 12 and VERSION96.image_length.offset == 16

    old = FRU192()
    old.schema_version, old.struct_size = 1, 192
    old.product_name, old.serial_number = b"B1-original-product", b"B1-SN-007"
    old.hardware_revision, old.manufacturing_date = b"D02", b"2026-08-01"
    old.mac_valid, old.network_mode, old.service_port = 1, 1, 8888
    old.mac_address[:] = bytes.fromhex("021122334455")
    old.configured_ipv4[:] = bytes([10, 20, 30, 47])
    old.configured_netmask[:] = bytes([255, 255, 255, 0])
    old.configured_gateway[:] = bytes([10, 20, 30, 1])
    old.site_label = b"SITE-7"
    converted = convert_fru(old)
    for field in ("product_name", "serial_number", "hardware_revision",
                  "manufacturing_date", "site_label", "service_port", "network_mode"):
        assert getattr(converted, field) == getattr(old, field), field
    for field in ("mac_address", "configured_ipv4", "configured_netmask", "configured_gateway"):
        assert bytes(getattr(converted, field)) == bytes(getattr(old, field)), field
    assert converted.tester_name == old.product_name and converted.test_board == b"RK3588_test"
    old.mac_valid = 0
    assert bytes(convert_fru(old).mac_address) == bytes(6)
    old.mac_valid = 1
    legacy160 = FRU160()
    legacy160.schema_version, legacy160.struct_size = 1, 160
    copy_common(old, legacy160)
    assert convert_fru(legacy160).serial_number == old.serial_number

    version = VERSION96()
    version.schema_version, version.struct_size = 1, 96
    version.version_major, version.version_minor, version.version_patch = 3, 4, 5
    version.image_length, version.image_crc32 = 123456, 0xABCDEF12
    version.build_time_utc, version.build_id, version.target = b"2026-08-31T01:02:03Z", b"Release", b"b1-h743"
    compact = VERSION()
    compact.schema_version, compact.struct_size = 1, 76
    copy_common(version, compact)
    assert compact.image_length == 123456 and compact.image_crc32 == 0xABCDEF12
    assert compact.target == b"b1-h743" and compact.version_patch == 5

    a, b = slot(old, 10), slot(legacy160, 11)
    assert write_target(a, b) == (0, 12)
    assert write_target(b, a) == (1, 12)
    assert write_target(slot(old, 0xFFFFFFFF), slot(legacy160, 0)) == (0, 1)
    assert write_target(slot(converted, 20), slot(old, 21)) == (0, 22)
    assert write_target(slot(version, 8, 1), slot(compact, 9, 1), 1) == (0, 10)
    assert read_choice(slot(converted, 10), slot(old, 11)) == ("legacy", 1, 11)
    assert read_choice(slot(old, 11), slot(converted, 10)) == ("legacy", 0, 11)
    assert read_choice(slot(old, 9), slot(converted, 10)) == ("current", 1, 10)
    assert read_choice(slot(old, 10), slot(converted, 10)) == ("current", 1, 10)
    assert read_choice(slot(converted, 10), slot(old, 10)) == ("current", 0, 10)

    # 反例：最新槽CRC有效，但字段语义非法；必须保留并返回另一份有效记录。
    for value in (converted, legacy160, old, compact, version):
        kind = 2 if hasattr(value, "network_mode") else 1
        good = slot(value, 10, kind)
        for name, field_type in value._fields_:
            if not (issubclass(field_type, ct.Array) and field_type._type_ is ct.c_char):
                continue
            broken = bytearray(bytes(value))
            offset = getattr(type(value), name).offset
            broken[offset:offset + ct.sizeof(field_type)] = b"X" * ct.sizeof(field_type)
            bad = slot(broken, 999, kind)  # 故意重算CRC，隔离字段有效性问题。
            assert probe(bad, kind)[0] == "invalid", name
            assert read_choice(good, bad, kind)[1:] == (0, 10), name
            assert write_target(good, bad, kind) == (1, 11), name
    for field, replacement in (("network_mode", 255), ("service_port", 0),
                               ("configured_ipv4", bytes(4)), ("configured_netmask", bytes(4))):
        broken = FRU.from_buffer_copy(bytes(converted))
        if isinstance(replacement, bytes):
            getattr(broken, field)[:] = replacement
        else:
            setattr(broken, field, replacement)
        bad = slot(broken, 999)
        assert probe(bad)[0] == "invalid", field
        assert read_choice(slot(converted, 10), bad) == ("current", 0, 10)
    bad_v2 = bytearray(bytes(converted))
    bad_v2[FRU.tester_name.offset:FRU.tester_name.offset + 32] = b"X" * 32
    assert read_choice(slot(old, 10), slot(bad_v2, 999)) == ("legacy", 0, 10)
    replacement = slot(converted, 12)
    for cut in range(32 + 224):
        interrupted = replacement[:cut] + b"\xff" * (4096 - cut)
        # 模拟目标扇区写入每个字节后掉电：未触碰的最新旧副本一直可用。
        assert probe(b) == ("valid", 11)
        assert write_target(interrupted, b) == (0, 12)
    assert write_target(replacement, b) == (1, 13)

    unknown = bytearray(bytes(converted))
    struct.pack_into("<H", unknown, 0, 9)
    for protected in (slot(unknown, 12), slot(converted, 12, format_version=2),
                      slot(struct.pack("<HH", 8, 300) + bytes(296), 12)):
        assert probe(protected)[0] == "unsupported"
        for pair in ((protected, b), (b, protected)):
            try:
                write_target(*pair)
            except ValueError:
                pass
            else:
                raise AssertionError("unknown valid slot was overwritten")
            try:
                read_choice(*pair)
            except ValueError:
                pass
            else:
                raise AssertionError("unknown valid slot allowed read fallback")
    corrupt = bytearray(slot(unknown, 12))
    corrupt[40] ^= 1
    assert probe(corrupt)[0] == "invalid"

    # 源码合同仅锁住关键入口，不能代替真实执行C代码。
    assert "FLASH_STORE_ERR_UNSUPPORTED" in HEADER
    assert "SLOT_UNSUPPORTED" in SOURCE and "check_unknown_record" in SOURCE
    slot_source = SOURCE.split("static slot_state_t read_slot(", 1)[1].split("static slot_state_t read_legacy_fru_slot(", 1)[0]
    assert slot_source.index("record_payload_is_valid(area, payload)") < slot_source.index("return SLOT_VALID")
    assert "memcpy(&aligned, payload, area->payload_size);" in SOURCE
    fru_load_source = SOURCE.split("int fru_info_load(", 1)[1].split("int fru_info_save(", 1)[0]
    assert "generation_is_newer(legacy_generation, current_generation)" in fru_load_source
    assert "return FLASH_STORE_ERR_LEGACY;" in fru_load_source
    assert "load_result != FLASH_STORE_ERR_LEGACY" in BOOT
    save_source = SOURCE.split("static int save_record(", 1)[1].split("int firmware_version_load(", 1)[0]
    assert save_source.index("read_legacy_slot") < save_source.index("flash->write(")
    assert save_source.index("FLASH_STORE_ERR_UNSUPPORTED") < save_source.index("flash->write(")
    assert "if (destination[0] != '\\0') return 0;" in BOOT
    assert "fru_info_load_legacy_v1" in BOOT and "firmware_version_current" not in QUERY
    assert "fru_info_load_legacy_v1" not in QUERY and "fru_info_save(" not in QUERY
    assert "fru_info_load(&flash_if, fru, NULL)" in QUERY
    initial_branch = BOOT.split("fru_info_init(fru);", 1)[1].split("fill_missing_fields", 1)[0]
    assert "if (mac != NULL) memcpy(fru->mac_address, mac, sizeof(fru->mac_address));" in initial_branch
    assert "ENABLE_FLASH_SELF_TEST" not in HEADER
    assert '#define FIRMWARE_TARGET "b1-h743"' in SOURCE
    print("B1 Flash legacy ABI/field/A-B/unknown-format model checks PASS (not C execution)")


if __name__ == "__main__":
    main()
