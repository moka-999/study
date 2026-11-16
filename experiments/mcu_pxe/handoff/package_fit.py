"""从固定D4双槽FIT生成H1参考包：仅等长替换BL33与SHA256，不重排、签名或烧录。"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / 'build/mcu_pxe'
BASE_SHA256 = '48202c6616521aac267de44e12fa259704597eb8192d4c5fe9b4013f6c488ae8'
ITS_SHA256 = 'e2cf6b4e2c202aa103294c63b5705170a746dc1676a98d31d9c5d130f5c3b5f3'
SLOT_SIZE = 2 * 1024 * 1024
BL33_SIZE = 983040
IMAGE_NAMES = ('uboot', 'atf-1', 'atf-2', 'atf-3', 'optee', 'fdt')
DATA_KEYS = {'data', 'data-size', 'data-position', 'data-offset'}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def u32(value):
    require(len(value) == 4, '要求一个32位大端单元')
    return int.from_bytes(value, 'big')


def fdt_parse(data):
    """只读FDT并记录属性原始偏移；所有长度受header与实际字节数共同限制。"""
    require(len(data) >= 40, 'FDT头不足40字节')
    header = struct.unpack_from('>10I', data)
    magic, total, structure, strings, reserve, version, compatible, _, strings_size, structure_size = header
    require(magic == 0xD00DFEED and version == 17 and compatible == 16, '不是本基线FDT版本')
    require(40 <= total <= len(data), 'FDT总长度越界')
    require(structure % 4 == 0 and structure + structure_size <= total and
            strings + strings_size <= total and reserve % 8 == 0, 'FDT块范围错误')
    reservations = []
    while True:
        require(reserve + 16 <= total, 'FDT reservation未结束')
        address, size = struct.unpack_from('>QQ', data, reserve)
        reserve += 16
        if address == 0 and size == 0:
            break
        reservations.append((address, size))
    nodes, stack = {}, []
    cursor, end = structure, structure + structure_size
    while cursor + 4 <= end:
        token = struct.unpack_from('>I', data, cursor)[0]
        cursor += 4
        if token == 1:
            nul = data.find(b'\0', cursor, end)
            require(nul >= 0, 'FDT节点名未结束')
            name = data[cursor:nul].decode('ascii')
            require(re.fullmatch(r'[A-Za-z0-9,._+?#@-]*', name), 'FDT节点名非法')
            stack.append(name)
            path = '/' + '/'.join(stack[1:])
            require(path not in nodes, 'FDT节点重复')
            nodes[path] = {}
            cursor = (nul + 4) & ~3
        elif token == 2:
            require(stack, 'FDT结束节点无配对')
            stack.pop()
        elif token == 3:
            require(stack and cursor + 8 <= end, 'FDT属性头越界')
            length, offset = struct.unpack_from('>II', data, cursor)
            cursor += 8
            require(cursor + length <= end and offset < strings_size, 'FDT属性越界')
            name_end = data.find(b'\0', strings + offset, strings + strings_size)
            require(name_end >= 0, 'FDT属性名未结束')
            name = data[strings + offset:name_end].decode('ascii')
            props = nodes['/' + '/'.join(stack[1:])]
            require(name not in props, 'FDT属性重复')
            props[name] = {'offset': cursor, 'value': data[cursor:cursor + length]}
            cursor = (cursor + length + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            require(not stack and '/' in nodes, 'FDT结构未闭合')
            return {'header': header, 'reservations': reservations, 'nodes': nodes}
        else:
            raise ValueError(f'未知FDT token {token}')
    raise ValueError('FDT缺少结束标记')


def values(tree, path):
    return {key: prop['value'] for key, prop in tree['nodes'][path].items()}


def check_unsigned(tree):
    placeholders = []
    for path, props in tree['nodes'].items():
        if path.rsplit('/', 1)[-1].startswith('signature'):
            require(not props.get('value', {}).get('value'), '存在签名value，禁止无密钥改包')
            require(not any(key in props for key in ('hashed-nodes', 'hashed-strings', 'signer-name')),
                    '存在已签名元数据，禁止无密钥改包')
            placeholders.append(path)
    return placeholders


def split_slots(image):
    require(len(image) == 2 * SLOT_SIZE, '要求4MiB、两个2MiB槽')
    require(image[:SLOT_SIZE] == image[SLOT_SIZE:], '两个FIT槽不一致')
    return image[:SLOT_SIZE]


def inspect_slot(slot):
    tree = fdt_parse(slot)
    check_unsigned(tree)
    names = tuple(path.split('/')[-1] for path in tree['nodes']
                  if path.startswith('/images/') and path.count('/') == 2)
    require(names == IMAGE_NAMES, 'FIT组件数量或顺序偏离固定基线')
    images, spans = {}, []
    for name in names:
        path = '/images/' + name
        props = values(tree, path)
        require(props.get('compression') == b'none\0', '本工具只允许基线未压缩组件')
        require('data-position' in props and 'data-size' in props and 'data' not in props and
                'data-offset' not in props, '要求固定data-position外置布局')
        position, length = u32(props['data-position']), u32(props['data-size'])
        require(length > 0 and position >= tree['header'][1] and position + length <= SLOT_SIZE,
                '组件越过FIT头或2MiB槽容量')
        payload = slot[position:position + length]
        digest = hashlib.sha256(payload).digest()
        hash_offsets, hash_nodes = [], []
        for child, child_props in tree['nodes'].items():
            if not child.startswith(path + '/') or child.count('/') != 3:
                continue
            algo = child_props.get('algo', {}).get('value')
            stored = child_props.get('value')
            if stored is not None:
                require(algo == b'sha256\0', '不支持的组件校验算法')
                require(stored['value'] == digest, f'{name}的{child} SHA256不匹配')
                hash_offsets.append(stored['offset'])
                hash_nodes.append(child)
        require(hash_offsets, f'{name}没有可验证的SHA256值')
        images[name] = {'position': position, 'size': length, 'sha256': digest.hex(),
                        'hash_offsets': hash_offsets, 'hash_nodes': hash_nodes,
                        'load': int.from_bytes(props['load'], 'big') if 'load' in props else None}
        spans.append((position, position + length))
    for previous, following in zip(sorted(spans), sorted(spans)[1:]):
        require(previous[1] <= following[0], '组件数据范围重叠')
    require(images['uboot']['size'] == BL33_SIZE and images['uboot']['load'] == 0x200000,
            'BL33尺寸/加载地址偏离已核对基线')
    return tree, images


def replace_bl33(base, fv):
    require(sha(base) == BASE_SHA256, 'D4基线SHA256不匹配，拒绝猜测其他FIT')
    slot = split_slots(base)
    tree, images = inspect_slot(slot)
    require(len(fv) == BL33_SIZE, '新BL33必须等长983040字节，拒绝截断或移动其他组件')
    require(fv[40:44] == b'_FVH', '新BL33缺少UEFI FV头')
    output = bytearray(slot)
    item = images['uboot']
    ranges = [(item['position'], item['position'] + item['size'])]
    output[ranges[0][0]:ranges[0][1]] = fv
    for offset in item['hash_offsets']:
        output[offset:offset + 32] = hashlib.sha256(fv).digest()
        ranges.append((offset, offset + 32))
    output = bytes(output)
    validate_unchanged(slot, output, ranges)
    new_tree, new_images = inspect_slot(output)
    for name in IMAGE_NAMES[1:]:
        require(images[name] == new_images[name], f'{name}不应变化')
    require(tree['header'] == new_tree['header'] and tree['reservations'] == new_tree['reservations'],
            'FDT头或reservation发生变化')
    require(output[item['position']:item['position'] + item['size']] == fv, 'BL33逐字节不一致')
    return output * 2, tree, new_images, sorted(ranges)


def validate_unchanged(old, new, ranges):
    require(len(old) == len(new), '槽长度变化')
    cursor = 0
    for begin, end in sorted(ranges):
        require(cursor <= begin < end <= len(old), '允许修改范围重叠或越界')
        require(old[cursor:begin] == new[cursor:begin], '允许范围外有字节变化')
        cursor = end
    require(old[cursor:] == new[cursor:], '允许范围外尾部有字节变化')


def linux_path(path):
    path = path.resolve()
    if sys.platform != 'win32':
        return str(path)
    require(re.match(r'^[A-Za-z]:', str(path)), 'WSL输入须为本机盘符路径')
    return '/mnt/' + path.drive[0].lower() + path.as_posix()[2:]


def tool(command, distribution):
    prefix = ['wsl', '-d', distribution, '--'] if sys.platform == 'win32' else []
    result = subprocess.run(prefix + command, capture_output=True, timeout=60)
    require(result.returncode == 0, result.stderr.decode('utf-8', errors='replace'))
    return result.stdout.decode('utf-8')


def negative_checks(base, fv):
    """只在内存构造失败情形，不写错误镜像，也不打开设备。"""
    checked = []
    def rejects(name, function):
        try:
            function()
        except ValueError:
            checked.append(name)
        else:
            raise ValueError('负向检查未拒绝：' + name)
    rejects('wrong baseline hash', lambda: replace_bl33(base[:-1] + b'\1', fv))
    rejects('unequal slots', lambda: split_slots(base[:-1] + b'\1'))
    rejects('truncated FDT', lambda: fdt_parse(base[:39]))
    rejects('changed BL33 size', lambda: replace_bl33(base, fv[:-1]))
    tree, images = inspect_slot(base[:SLOT_SIZE])
    signed = copy.deepcopy(tree)
    signature = check_unsigned(signed)[0]
    signed['nodes'][signature]['value'] = {'value': bytes(256), 'offset': 0}
    rejects('real signature value', lambda: check_unsigned(signed))
    bad_hash = bytearray(base[:SLOT_SIZE])
    bad_hash[images['uboot']['position']] ^= 1
    rejects('component hash mismatch', lambda: inspect_slot(bytes(bad_hash)))
    bad_range = bytearray(base[:SLOT_SIZE])
    offset = tree['nodes']['/images/uboot']['data-position']['offset']
    bad_range[offset:offset + 4] = (SLOT_SIZE - 4).to_bytes(4, 'big')
    rejects('component crosses slot', lambda: inspect_slot(bytes(bad_range)))
    rejects('unexpected metadata mutation', lambda: validate_unchanged(b'abc', b'xbc', [(1, 2)]))
    return checked


def write_reference_its(template, tree, destination):
    """ITS用于审阅/另行重建，不用它重排本次严格保留字节布局的候选。"""
    text = template.decode('utf-8-sig')
    require(text.count('/dts-v1/;') == 1 and text.count('/ {') == 1, '参考ITS根节点不唯一')
    reservations = ''.join(f'\n/memreserve/ 0x{address:x} 0x{size:x};' for address, size in tree['reservations'])
    text = text.replace('/dts-v1/;', '/dts-v1/;' + reservations, 1)
    timestamp = u32(values(tree, '/')['timestamp'])
    text = text.replace('/ {', f'/ {{\n\ttimestamp = <0x{timestamp:x}>;', 1)
    for name in IMAGE_NAMES:
        old = f'/incbin/("{name}")'
        require(text.count(old) == 1, '参考ITS组件不唯一：' + name)
        text = text.replace(old, f'/incbin/("components/{name}")', 1)
    destination.write_text(text, encoding='utf-8', newline='\n')


def metadata_without_generated(tree):
    result = {}
    for path in tree['nodes']:
        props = values(tree, path)
        result[path] = {key: value for key, value in props.items()
                        if key not in DATA_KEYS and key != 'value'}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, default=BUILD / 'artifacts/b1_serial_net_D4_network_20261103.img')
    parser.add_argument('--its', type=Path, default=BUILD / 'artifacts/reference-fit.its')
    parser.add_argument('--fv', type=Path, default=BUILD / 'handoff/artifacts/BL33_AP_UEFI.Fv')
    parser.add_argument('--out', type=Path, default=BUILD / 'handoff/package_20261110')
    parser.add_argument('--distribution', default='Ubuntu-26.04')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    inputs = [path.resolve() for path in (args.base, args.its, args.fv)]
    before = {str(path): sha(path.read_bytes()) for path in inputs}
    base, template, fv = (path.read_bytes() for path in inputs)
    require(sha(template) == ITS_SHA256, '参考ITS SHA256不匹配')
    candidate, tree, images, ranges = replace_bl33(base, fv)
    checks = negative_checks(base, fv)
    if args.self_test:
        print('PASS:', len(checks), 'negative checks')
        return
    out = args.out.resolve()
    allowed = (BUILD / 'handoff').resolve()
    require(out.is_relative_to(allowed) and out != allowed and
            not any(path.is_relative_to(out) for path in inputs), '输出必须独立于输入并位于handoff构建目录')
    out.mkdir(parents=True, exist_ok=True)
    components = out / 'components'
    components.mkdir(exist_ok=True)
    for name, info in images.items():
        (components / name).write_bytes(candidate[info['position']:info['position'] + info['size']])
    its = out / 'uboot_h1_reference.its'
    write_reference_its(template, tree, its)
    (out / 'reference-fit.input.its').write_bytes(template)
    temporary = out / 'uboot_h1_reference.candidate'
    temporary.write_bytes(candidate)
    (out / 'fit-info.txt').write_text(tool(['dumpimage', '-l', linux_path(temporary)], args.distribution), encoding='utf-8')
    (out / 'actual-metadata.dts').write_text(tool(['dtc', '-I', 'dtb', '-O', 'dts', linux_path(temporary)], args.distribution), encoding='utf-8')
    extracted = out / 'independent-extraction'
    extracted.mkdir(exist_ok=True)
    for index, name in enumerate(IMAGE_NAMES):
        path = extracted / name
        tool(['dumpimage', '-T', 'flat_dt', '-p', str(index), '-o', linux_path(path), linux_path(temporary)], args.distribution)
        require(path.read_bytes() == (components / name).read_bytes(), 'dumpimage独立提取不一致：' + name)
    inline = out / 'reference-inline.dtb'
    tool(['dtc', '-I', 'dts', '-O', 'dtb', '-o', linux_path(inline), linux_path(its)], args.distribution)
    inline_tree = fdt_parse(inline.read_bytes())
    require(metadata_without_generated(tree) == metadata_without_generated(inline_tree) and
            tree['reservations'] == inline_tree['reservations'], '参考ITS和实际FIT的元数据不一致')
    for name in IMAGE_NAMES:
        require(values(inline_tree, '/images/' + name)['data'] == (components / name).read_bytes(),
                '参考ITS的组件数据不一致：' + name)
    require(before == {str(path): sha(path.read_bytes()) for path in inputs}, '输入文件发生变化')
    final = out / 'uboot_h1_reference.img'
    temporary.replace(final)
    report = {
        'method': 'fixed-baseline, same-size BL33 replacement; no FIT restructuring or signing',
        'inputs_sha256': before, 'inputs_unchanged': True,
        'output': {'file': str(final), 'size': len(candidate), 'sha256': sha(candidate)},
        'slot_size': SLOT_SIZE, 'slots_identical': split_slots(candidate) == candidate[:SLOT_SIZE],
        'modified_ranges_per_slot': [{'begin': start, 'end_exclusive': end} for start, end in ranges],
        'all_other_bytes_unchanged': True, 'all_components_sha256_verified': True,
        'new_bl33_matches_fv_byte_for_byte': True, 'dumpimage_all_components_match': True,
        'metadata_and_reference_its_match': True, 'reservations_preserved': tree['reservations'],
        'timestamp_inherited_not_build_time': u32(values(tree, '/')['timestamp']),
        'declared_fit_totalsize_preserved': u32(values(tree, '/')['totalsize']),
        'signature': {'signed': False, 'placeholder_nodes': check_unsigned(tree),
                      'note': 'Only signature algorithm/key hints exist; no signature value. Not re-signed.'},
        'components': images, 'negative_checks_passed': checks,
        'tools': {name: tool([name, '-V' if name != 'dtc' else '--version'], args.distribution).strip()
                  for name in ('dumpimage', 'dtc')},
        'limitations': ['Reference FIT only; no update.img, Loader merge, flashing, or network service.',
                       'ATF/TEE/DTB inherited unchanged from old D4; compatibility with the new Loader is unverified.',
                       'Hardware execution, secure-boot acceptance, DHCP and MemTest86 are not verified here.']}
    (out / 'verification.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report['output'], ensure_ascii=False))
    print('PASS: six component hashes, unchanged non-BL33 bytes, dual slots, inputs and independent extraction')


if __name__ == '__main__':
    main()
