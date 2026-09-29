"""只读核对网络助手算法清单；不访问网络、串口或烧录器。"""

import json
from pathlib import Path
import re
import struct


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "App/tasks/rk/rk_link.c").read_text(encoding="utf-8")
    table = source.split("s_command_table[] = {", 1)[1].split("};", 1)[0]
    algorithms = re.findall(r'\{\s*(\d+)U,\s*"([^"]+)"\s*\}', table)
    document = (root / "doc/func_algorithm_network_packets.md").read_text(encoding="utf-8")
    examples = re.findall(
        r"```json\n(.*?)\n```\n\n完整 HEX：(\d+) 字节，示例 Seq=(\d+)。\n\n```text\n(.*?)\n```",
        document,
        re.S,
    )
    assert len(algorithms) == len(examples) == 20, "算法表或示例数量不符"
    rows = re.findall(
        r"^\| (\d+) \| `([^`]+)` \| [^|]+ \| ([+-]1) \| (\d+) \| (\d+) \|$",
        document,
        re.M,
    )
    assert len(rows) == 20, "目录必须列出20项档位及报文长度"

    # index 和 Seq 在本清单按顺序取值，并非固件要求它们等于 RK 命令号。
    for (command, name), (json_text, size, seq, hex_text), row in zip(algorithms, examples, rows):
        index = int(command)
        step = 1 if index <= 10 else -1
        body = json_text.encode("utf-8")
        assert re.fullmatch(r"[0-9A-F]+", hex_text), "HEX必须单行且无空格，避免网络助手粘贴断线"
        packet = bytes.fromhex(hex_text)
        fields = struct.unpack(">IHHHHIH", packet[:18])
        assert fields == (len(packet), 1, 8, 0, 0x0103, index, 0), name
        assert len(packet) == 18 + len(body) <= 4096, name
        assert int(size) == len(packet) and int(seq) == index, name
        assert row == (command, name, f"{step:+d}", command, size), name
        assert packet[18:] == body, name
        assert json.loads(body) == {
            "algorithmType": "functional",
            "algorithmName": name,
            "algorithmIndex": index,
            "power": {
                "memoryType": "LPDDR4X",
                "vdd1Steps": step,
                "vdd2Steps": step,
                "vddqSteps": step,
            },
            "reboot": False,
            "bootMode": "emmc",
        }, name

    # 轮询包不携带算法参数；示例2、3仅适用于此前只发过Seq=1开始包的连接。
    polling = document.split("## 测试期间持续查询", 1)[1].split("## 回复怎么看", 1)[0]
    queries = re.findall(r"```text\n([0-9A-F]+)\n```", polling)
    assert len(queries) == 2, "缺少两次递增Seq的0601查询示例"
    for seq, hex_text in enumerate(queries, 2):
        packet = bytes.fromhex(hex_text)
        assert struct.unpack(">IHHHHIH", packet[:18]) == (20, 1, 8, 0, 0x0601, seq, 0)
        assert len(packet) == 20 and packet[18:] == b"{}"
    print("通过：20组算法、三轨前10项+1/后10项-1、JSON/单行HEX/目录长度、包头及2个0601查询包；未发送任何报文。")


if __name__ == "__main__":
    main()
