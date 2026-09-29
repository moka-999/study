"""单次真实FUNC网络复现；默认离线，只在--execute时发一次算法，不自动重试。"""
import argparse
import json
from pathlib import Path
import re
import socket
import struct
import time

from pc_business_network_test import recv_exact


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--host", default="192.168.1.201")
    parser.add_argument("--port", type=int, default=8888)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    document = (root / "doc/func_algorithm_network_packets.md").read_text(encoding="utf-8")
    body_text, hex_text = re.findall(
        r"```json\n(.*?)\n```.*?```text\n(.*?)\n```", document, re.S)[0]
    packet = bytes.fromhex(hex_text)
    assert packet[18:] == body_text.encode("utf-8")
    assert struct.unpack(">IHHHHIH", packet[:18]) == (149, 1, 8, 0, 0x0103, 1, 0)
    body = json.loads(body_text)
    assert body == dict(algorithmType="functional", algorithmName="gtest-stuck-address",
                        algorithmIndex=1, power=None, reboot=False, bootMode=None)
    if not args.execute:
        print("离线校验通过：149字节，gtest-stuck-address，未连接开发板。")
        return
    if args.report is None or args.report.exists():
        parser.error("执行必须指定不存在的--report，禁止覆盖诊断证据")
    report = {"scope": "真实RK算法仅发送一次；不改电源/启动，不复位、不升级",
              "host": args.host, "port": args.port, "events": []}
    started = time.monotonic()

    def record(event):
        # 每个阶段立即落盘；即使算法一直不返回，也保留已受理和原始字节证据。
        event["elapsedMs"] = round((time.monotonic() - started) * 1000, 3)
        report["events"].append(event)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps(event, ensure_ascii=False), flush=True)

    try:
        with socket.create_connection((args.host, args.port), timeout=3) as connection:
            connection.settimeout(5)
            record({"event": "connected"})
            connection.sendall(packet)
            record({"event": "sent_once", "bytes": len(packet), "hex": packet.hex(" "), "json": body})
            accepted = False
            while True:
                header = recv_exact(connection, 18)
                fields = struct.unpack(">IHHHHIH", header)
                assert 18 <= fields[0] <= 4096 and fields[1:4] == (1, 8, 0), fields
                payload = recv_exact(connection, fields[0] - 18)
                response = json.loads(payload)
                record({"event": "received", "command": fields[4], "seq": fields[5],
                        "hex": (header + payload).hex(" "), "json": response})
                if fields[4] == 0x0105:
                    assert accepted and response["algorithmIndex"] == 1
                    break
                assert fields[4] == 0x0006, fields
                if response.get("status") != "accepted":
                    break
                accepted = True
                # 这是PC观察上限，不是向MCU下发的算法超时；到期不会取消或重发RK。
                connection.settimeout(1200)
    except Exception as error:
        record({"event": "error", "detail": repr(error),
                "note": "不自动重连重发；若已发送或受理，须确认RK实际状态"})
    finally:
        record({"event": "socket_closed"})


if __name__ == "__main__":
    main()
