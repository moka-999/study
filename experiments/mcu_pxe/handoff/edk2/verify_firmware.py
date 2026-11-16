"""展开本机 BL33 固件卷，核验 H1 模块、应用 UI 和 AArch64 产物；不访问设备。"""
import argparse
import hashlib
import json
import lzma
from pathlib import Path
import struct
import uuid


MODULES = {
    'SerialNetSnpDxe': '5d9a5076-d58e-4f1a-991c-bef1681ce6a4',
    'NetReturn': '4c701a74-edc5-4994-8f1a-b923a18e61cd',
    'FvSimpleFileSystem': '907125c0-a5f1-11e3-a3fe-a3198b49350c',
    'BdsDxe': '6d33944a-ec75-4855-a54d-809c75241f6c',
    'UefiPxeBcDxe': 'b95e9fda-26de-48d2-8807-1f9107ac5e3a',
}
LZMA_GUID = uuid.UUID('ee4e5898-3914-4259-9d6e-dc7bd79403cf').bytes_le
MARKER = 'B1-SERIAL-NET-20261112-H1C'
AUTO_MARKER = 'SLT-AUTO-20261111'


def sections(data):
    """按 PI section 长度前进；只允许最多三字节的对齐尾部。"""
    pos = 0
    while pos + 4 <= len(data):
        size = int.from_bytes(data[pos:pos + 3], 'little')
        header = 4
        if size == 0xffffff:
            size = struct.unpack_from('<I', data, pos + 4)[0]
            header = 8
        if size < header or pos + size > len(data):
            raise ValueError('固件 section 长度越界')
        yield data[pos + 3], data[pos + header:pos + size], header
        pos = (pos + size + 3) & ~3


def files_in_fv(data):
    """按 FV/FFS 实际头部解析文件，避免把压缩内容的字节碰巧匹配认作模块。"""
    if data[40:44] != b'_FVH':
        raise ValueError('缺少 FV 签名')
    size = struct.unpack_from('<Q', data, 32)[0]
    if size > len(data):
        raise ValueError('FV 长度越界')
    pos = struct.unpack_from('<H', data, 48)[0]
    ext = struct.unpack_from('<H', data, 52)[0]
    if ext:
        pos = max(pos, ext + struct.unpack_from('<I', data, ext + 16)[0])
    pos = (pos + 7) & ~7
    while pos + 24 <= size and data[pos:pos + 24] != b'\xff' * 24:
        length = int.from_bytes(data[pos + 20:pos + 23], 'little')
        header = 24
        if length == 0xffffff:
            length = struct.unpack_from('<Q', data, pos + 24)[0]
            header = 32
        if length < header or pos + length > size:
            raise ValueError('FFS 长度越界')
        if data[pos + 18] != 0xf0:
            yield str(uuid.UUID(bytes_le=data[pos:pos + 16])), data[pos + header:pos + length]
        pos = (pos + length + 7) & ~7


def collect_fvs(data):
    """展开外层 LZMA GUIDED section，收集真实嵌套 FV。"""
    volumes = [data]
    for _, payload in files_in_fv(data):
        for kind, content, header in sections(payload):
            if kind != 2 or content[:16] != LZMA_GUID:
                continue
            offset = struct.unpack_from('<H', content, 16)[0] - header
            expanded = lzma.decompress(content[offset:], format=lzma.FORMAT_ALONE)
            for inner_kind, inner, _ in sections(expanded):
                if inner_kind == 0x17:
                    volumes.extend(collect_fvs(inner))
    return volumes


def verify(fv_path, app_path):
    """核验最终 BL33 与单独交付的应用一致，返回可记录的结构证据。"""
    data, app = fv_path.read_bytes(), app_path.read_bytes()
    volumes = collect_fvs(data)
    found = {}
    total = 0
    for volume in volumes:
        for guid, payload in files_in_fv(volume):
            total += 1
            for name, expected in MODULES.items():
                if guid != expected:
                    continue
                ui, pe = None, None
                for kind, content, _ in sections(payload):
                    if kind == 0x15:
                        ui = content.decode('utf-16-le').rstrip('\0')
                    elif kind == 0x10:
                        pe = content
                if name in found or ui != name or pe is None:
                    raise ValueError(f'{name} 重复或缺少正确 UI/PE32 section')
                pe_header = struct.unpack_from('<I', pe, 0x3c)[0]
                if pe[pe_header:pe_header + 4] != b'PE\0\0' or struct.unpack_from('<H', pe, pe_header + 4)[0] != 0xaa64:
                    raise ValueError(f'{name} 不是 AArch64 PE 映像')
                if name == 'NetReturn' and pe != app:
                    raise ValueError('FV 内 NetReturn 与单独应用不一致')
                if name == 'BdsDxe' and AUTO_MARKER.encode('utf-16-le') not in pe:
                    raise ValueError('BdsDxe未链接本次自动PXE/归还入口')
                if name == 'UefiPxeBcDxe' and b'SerialNet: DHCP requested IP=' not in pe:
                    raise ValueError('PXE未链接本次授权IP请求入口')
                if name == 'SerialNetSnpDxe' and b'SerialNet: UART data baud=' not in pe:
                    raise ValueError('SNP未链接本次速率协商入口')
                found[name] = {'guid': guid, 'ui': ui, 'machine': 'AArch64', 'pe_size': len(pe)}
    if set(found) != set(MODULES):
        raise ValueError('最终 BL33 缺少 H1 必需模块')
    if not any(MARKER.encode('utf-16-le') in v or MARKER.encode('ascii') in v for v in volumes):
        raise ValueError('最终 BL33 缺少 H1 版本标记')
    return {'status': 'PASS', 'marker': MARKER, 'fv_count': len(volumes), 'ffs_count': total,
            'modules': found, 'artifacts': [
                {'path': str(p), 'size': len(b), 'sha256': hashlib.sha256(b).hexdigest()}
                for p, b in ((fv_path, data), (app_path, app))]}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fv', type=Path)
    parser.add_argument('app', type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.fv, args.app), ensure_ascii=False, indent=2))
