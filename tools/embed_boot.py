"""把独立Boot固定在最终ELF的首扇区；不包含升级协议或外置固件包。"""
from pathlib import Path
import struct
import sys

def embed(source, output):
    data = source.read_bytes()
    if not 664 <= len(data) <= 0x20000:
        raise ValueError('Boot必须包含完整向量且不超过128KiB')
    stack, entry = struct.unpack_from('<II', data)
    if stack != 0x20020000 or not entry & 1 or not 0x08000298 <= (entry & ~1) < 0x08000000 + len(data):
        raise ValueError('Boot向量或入口异常')
    output.write_text('/* 自动生成：只嵌入独立Boot，不修改App向量。 */\n'
                      '.section .boot_image,"ax",%progbits\n.balign 4\n'
                      f'.incbin "{source.resolve().as_posix()}"\n'
                      f'.global Boot_Image_Reset\n.set Boot_Image_Reset, 0x{entry:08x}\n', encoding='utf-8')

if __name__ == '__main__':
    embed(Path(sys.argv[1]), Path(sys.argv[2]))
