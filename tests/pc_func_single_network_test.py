"""单次真实FUNC网络复现；默认离线，只在--execute时发一次算法，不自动重试。"""
import argparse
import json
from pathlib import Path
import socket
import struct
import time

from pc_business_network_test import make_frame, recv_exact, recv_frame


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--host", default="192.168.1.201")
    parser.add_argument("--port", type=int, default=8888)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    # 单次复现只用外部已启动模式，不借用文档中会改变模拟电压的20组示例。
    body = dict(algorithmType="functional", algorithmName="gtest-stuck-address",
                algorithmIndex=1, power=None, reboot=False, bootMode=None)
    packet = make_frame(0x0103, json.dumps(body, separators=(",", ":")).encode(), 2)
    assert struct.unpack(">IHHHHIH", packet[:18]) == (149, 1, 8, 0, 0x0103, 2, 0)
    if not args.execute:
        print("离线校验通过：149字节，gtest-stuck-address，未连接开发板。")
        return
    if args.report is None or args.report.exists():
        parser.error("执行必须指定不存在的--report，禁止覆盖诊断证据")
    report = {"scope": "先0106，真实RK算法仅一次，完成后0107；更新灯，不改电源/启动、不复位升级",
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
            connection.sendall(make_frame(0x0106, b"{}", 1))
            fields, response = recv_frame(connection)
            record({"event": "round_begin", "header": fields, "json": response})
            assert fields[4] == 6 and response == {"requestCommandId": 0x0106, "status": "completed"}, response
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
                    failures = [{"physicalAddr": chip["physicalAddr"], "mirrorIndex": chip["mirrorIndex"]}
                                for chip in response["testResults"] if chip["testStatus"] == 0]
                    summary = json.dumps({"errorchip": failures}, separators=(",", ":")).encode()
                    connection.sendall(make_frame(0x0107, summary, 3))
                    fields, summary_response = recv_frame(connection)
                    record({"event": "round_end", "header": fields, "json": summary_response})
                    assert fields[4] == 6 and summary_response == {"requestCommandId": 0x0107, "status": "completed"}
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
