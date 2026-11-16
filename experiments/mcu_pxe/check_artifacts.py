"""只读核验MCU合并镜像和RK参考FIT，不访问开发板。"""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parent
ARTIFACTS = ROOT.parents[1] / 'build/mcu_pxe/artifacts'


def fit_properties(data):
    """仅读取FDT结构块中的节点属性；FIT有效性还需SPL实际加载验收。"""
    magic, size, tree, strings, _, _, _, _, strings_size, tree_size = struct.unpack_from('>10I', data)
    assert magic == 0xD00DFEED and size <= len(data)
    assert tree + tree_size <= size and strings + strings_size <= size
    pos, stack, props = tree, [], {}
    while pos < tree + tree_size:
        token = struct.unpack_from('>I', data, pos)[0]; pos += 4
        if token == 1:
            end = data.index(0, pos, tree + tree_size)
            stack.append(data[pos:end].decode('ascii')); pos = (end + 4) & ~3
        elif token == 2:
            stack.pop()
        elif token == 3:
            length, name = struct.unpack_from('>II', data, pos); pos += 8
            end = data.index(0, strings + name, strings + strings_size)
            key = data[strings + name:end].decode('ascii')
            assert pos + length <= tree + tree_size
            props[('/'.join(stack), key)] = data[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            return size, props
        else:
            raise ValueError('未知FDT token')
    raise ValueError('FDT未结束')


def main():
    spec = importlib.util.spec_from_file_location('direct_flash_check', ROOT.parents[1] / 'tools/direct_flash.py')
    checker = importlib.util.module_from_spec(spec); spec.loader.exec_module(checker)
    image = checker.check_image(ARTIFACTS / 'b1_serial_net.bin', 'b1-h743')
    boot_sp, boot_pc = struct.unpack_from('<II', image)
    app_sp, app_pc = struct.unpack_from('<II', image, 0x20000)
    fit = (ARTIFACTS / 'b1_serial_net_reference_fit.img').read_bytes()
    slot = 2 * 1024 * 1024
    assert len(fit) == slot * 2 and fit[:slot] == fit[slot:]
    fdt_size, props = fit_properties(fit)
    node = '/images/uboot'
    value = lambda key: int.from_bytes(props[node, key], 'big')
    length = value('data-size')
    position = value('data-position') if (node, 'data-position') in props else ((fdt_size + 3) & ~3) + value('data-offset')
    bl33 = (ARTIFACTS / 'b1_serial_net_bl33.bin').read_bytes()
    assert position + length <= slot and fit[position:position + length] == bl33
    assert len(bl33) == 960 * 1024 and bl33[40:44] == b'_FVH'
    assert value('load') == 0x00200000
    assert props['/images/uboot/hash', 'algo'] == b'sha256\0'
    assert props['/images/uboot/hash', 'value'] == hashlib.sha256(bl33).digest()
    assert 'B1-SERIAL-NET-20261103'.encode('utf-16le') in bl33
    manifest = {p.name: {'size': p.stat().st_size, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                for p in ARTIFACTS.iterdir() if p.suffix in ('.bin', '.img', '.elf')}
    (ARTIFACTS / 'SHA256.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    result = {'mcu_bytes': len(image), 'boot_sp': hex(boot_sp), 'boot_pc': hex(boot_pc),
              'app_sp': hex(app_sp), 'app_pc': hex(app_pc), 'bl33_bytes': len(bl33),
              'bl33_load': hex(value('load')), 'fit_copies_equal': True,
              'fit_bl33_hash_matches': True, 'hardware_tested': False}
    (ARTIFACTS / 'verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print('PASS: MCU Boot+App ELF/BIN一致；FIT双副本及BL33散列一致；未连接硬件。')


if __name__ == '__main__':
    main()
