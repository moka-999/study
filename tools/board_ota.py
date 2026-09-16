#!/usr/bin/env python3
"""V2 SWD备份/维护工具。只连接指定ST-LINK服务器，写操作要求本轮备份目录完整。

RAM助手源码在tests/ram_agent.c；这里不安装驱动、不修改选项字节、不作网络下载。
通过标准GDB批处理调度，无需GDB内置Python。实际写入前仍须获得操作者授权。
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GNU = Path("C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin")


def run_script(directory: Path, name: str, commands: list[str], timeout: int = 300) -> str:
    """保存本次实际GDB命令与原始输出便于审计；失败不吞掉或自动重试写操作。"""
    script = directory / (name + ".gdb")
    script.write_text("\n".join(commands + ["detach", "quit"]) + "\n", encoding="utf-8")
    result = subprocess.run([str(GNU / "arm-none-eabi-gdb.exe"), "-nx", "-q", "--batch", "-x", str(script)],
                            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
    output = result.stdout + result.stderr
    (directory / (name + ".log")).write_text(output, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"GDB失败({result.returncode})，见{directory / (name + '.log')}\n{output[-2500:]}")
    return output


def begin_agent() -> list[str]:
    """每次都复位后装入RAM助手；绝不执行Flash加载。"""
    agent = (ROOT / "build/Debug/boot_ram_agent.elf").as_posix()
    return ["set pagination off", "set confirm off", "set remotetimeout 20",
            f'file "{agent}"', "target extended-remote localhost:61234", "monitor reset", "monitor halt",
            "load", "set $pc = Reset_Handler", "set $sp = 0x20020000", "set $xpsr = 0x01000000",
            "hbreak agent_checkpoint", "continue",
            "if agent_mailbox[0] != 0x4147454E", "quit 2", "end"]


def command(code: int, address: int, count: int, *, write: bool = False) -> list[str]:
    result = [f"set agent_mailbox[2] = {address}", f"set agent_mailbox[3] = {count}"]
    if write:
        result.append("set agent_mailbox[6] = 0x53574457")
    return result + [f"set agent_mailbox[1] = {code}", "continue",
                     "if agent_mailbox[1] != 0 || agent_mailbox[4] != 0", "p/x agent_mailbox", "p/x boot_diag", "quit 3", "end"]


def read_commands(directory: Path, prefix: str, address: int, length: int) -> tuple[list[str], list[Path]]:
    commands, chunks = [], []
    for offset in range(0, length, 65536):
        count = min(65536, length - offset)
        path = directory / f"{prefix}-{offset:06x}.bin"
        chunks.append(path)
        commands += command(1, address + offset, count)
        commands.append(f'dump binary memory {path.as_posix()} agent_buffer (agent_buffer+{count})')
    return commands, chunks


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("backup", "read", "write", "record"))
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--address", type=lambda s: int(s, 0), default=0)
    parser.add_argument("--length", type=lambda s: int(s, 0), default=0x6000)
    parser.add_argument("--input", type=Path)
    parser.add_argument("--name", default="operation")
    args = parser.parse_args()
    directory = args.directory.resolve()
    if any(c.isspace() or c == '"' for c in str(directory)):
        raise ValueError("此版GDB dump/restore要求备份目录不含空白或引号")
    if not directory.is_dir():
        raise ValueError("先建立本次备份目录并通过CubeProgrammer读取original-internal.bin")
    if any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for c in args.name):
        raise ValueError("name只允许字母数字下划线横线")
    if (directory / (args.name + ".gdb")).exists():
        raise ValueError("操作名称已使用，保留原始证据，请换name")
    commands = begin_agent()
    if args.action == "backup":
        internal = directory / "original-internal.bin"
        if internal.stat().st_size != 0x200000 or (directory / "backup.json").exists():
            raise ValueError("内部Flash备份缺失/大小不对或已存在备份，禁止覆盖")
        passes = []
        for label in ("nor-first", "nor-second"):
            reads, chunks = read_commands(directory, label, 0, 0x410000)
            commands += reads
            passes.append(chunks)
        run_script(directory, args.name, commands)
        data = [b"".join(path.read_bytes() for path in parts) for parts in passes]
        if len(data[0]) != 0x410000 or data[0] != data[1]:
            raise ValueError("两次W25读取不一致，不允许后续写入")
        nor = directory / "original-w25-0-410000.bin"
        nor.write_bytes(data[0])
        manifest = {"scope": "internal 2MiB and W25 affected first 0x410000 bytes",
                    "internal_sha256": sha(internal), "w25_sha256": sha(nor),
                    "fru_sha256": hashlib.sha256(data[0][0x2000:0x4000]).hexdigest(),
                    "ram_agent_sha256": sha(ROOT / "build/Debug/boot_ram_agent.elf")}
        (directory / "backup.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print("PASS：内部Flash及W25相关区域备份完成，W25两次读取逐字节一致。", flush=True)
    elif args.action == "read":
        if args.address < 0 or args.length <= 0 or args.address + args.length > 0x2000000:
            raise ValueError("NOR读取范围非法")
        reads, chunks = read_commands(directory, args.name + "-read", args.address, args.length)
        run_script(directory, args.name, commands + reads)
        destination = directory / (args.name + ".bin")
        destination.write_bytes(b"".join(p.read_bytes() for p in chunks))
        print(f"PASS：读取{destination}，SHA256={sha(destination)}")
    else:
        manifest = json.loads((directory / "backup.json").read_text())
        if sha(directory / "original-internal.bin") != manifest["internal_sha256"] or \
           sha(directory / "original-w25-0-410000.bin") != manifest["w25_sha256"]:
            raise ValueError("本轮备份校验失败，拒绝写入")
        if args.input is None:
            raise ValueError("写操作需要--input")
        if any(c.isspace() or c == '"' for c in str(args.input.resolve())):
            raise ValueError("GDB restore输入路径不能包含空白或引号")
        data = args.input.read_bytes()
        if args.action == "record":
            if len(data) != 236:
                raise ValueError("boot_record_t必须是236字节；持久化仍由C编码而非直接写结构体")
            commands += [f'restore {args.input.resolve().as_posix()} binary agent_buffer']
            commands += command(5, 0, len(data), write=True)
        else:
            if not data or len(data) % 4096 or args.address < 0 or args.address % 4096 or args.address + len(data) > 0x410000:
                raise ValueError("写入起点和长度必须4KiB对齐并完全位于已备份范围，禁止擦掉输入以外的尾页数据")
            for offset in range(0, len(data), 4096):
                commands += command(3, args.address + offset, 4096, write=True)
                chunk = directory / f"{args.name}-write-{offset:06x}.bin"
                chunk.write_bytes(data[offset:offset + 4096])
                commands += [f'restore {chunk.as_posix()} binary agent_buffer']
                commands += command(2, args.address + offset, len(data[offset:offset + 4096]), write=True)
        run_script(directory, args.name, commands)
        print(f"PASS：{args.action}完成，目标0x{args.address:X}，输入{len(data)}字节；详见操作日志。")


if __name__ == "__main__":
    main()
