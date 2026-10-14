"""验证V2查询/拒绝及整轮路径；会更新结果灯，不发算法、不改电源或复位。"""
import argparse
import json
from pathlib import Path
import socket

from pc_ota_network_test import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.1.201")
    parser.add_argument("--port", type=int, default=8888)
    parser.add_argument("--version", required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error("报告已存在，换名称以保留证据")
    report = {"scope": "查询、拒绝及整轮灯态；未执行RK算法", "events": []}
    try:
        with socket.create_connection((args.host, args.port), timeout=3) as sock:
            sock.settimeout(5)
            client = Client(sock, report["events"])
            _, state = client.call({"action": "mcu_query"})
            assert state["confirmed"] and state["firmwareVersion"] == args.version, state
            client.call({}, command=0x0002, expect=0x0001)
            client.call({}, command=0x0101, expect=0x0102)
            _, result = client.call({}, command=0x0106, expect=6)
            assert result.get("status") == "completed", result
            # 故意使用不存在的名字，即使后续字段放行，也不能查表下发真实算法。
            body = dict(algorithmType="functional", algorithmName="__NO_SUCH_ALGORITHM__",
                        algorithmIndex=123, power=None, reboot=False, bootMode=None)
            client.call(body, command=0x0103, expect=6, error="notSupported")
            client.call(dict(body, algorithmType="parametric"), command=0x0103,
                        expect=6, error="notSupported")
            client.call(dict(body, reboot=True), command=0x0103,
                        expect=6, error="invalidArgument")
            client.call(dict(body, bootMode="emmc"), command=0x0103,
                        expect=6, error="invalidArgument")
            client.call(dict(body, timeoutMs=0), command=0x0103,
                        expect=6, error="notSupported")
            client.call(dict(body, timeoutMs=1.5), command=0x0103,
                        expect=6, error="invalidArgument")
            client.call(dict(body, power=dict(memoryType="LPDDR4X", vdd1="1800",
                        vdd2="1100", vddq="600"), bootMode="emmc"), command=0x0103,
                        expect=6, error="notSupported")
            client.call({}, command=0x0002, expect=0x0001)
            _, result = client.call({"errorchip": []}, command=0x0107, expect=6)
            assert result.get("status") == "completed", result
            report["passed"] = True
            print("PASS: 版本/健康、心跳、W25信息、轮次与拒绝路径；三灯保留汇总PASS")
            print("未下发RK算法，真实执行和结果回传仍待操作者发起")
    except BaseException as error:
        report["error"] = repr(error)
        raise
    finally:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
