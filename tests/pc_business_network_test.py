#!/usr/bin/env python3
"""Apollo V2上验证PC业务帧，不查询/写入W25Q256，也不访问真实TPS/PAC/AD。"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import time


HEADER_SIZE = 18
VERSION = 1
FLAG_JSON = 8


def make_frame(command: int, body: bytes, sequence: int,
               service: int = 0) -> bytes:
    return struct.pack(
        ">IHHHHIH", HEADER_SIZE + len(body), VERSION, FLAG_JSON,
        service, command, sequence, 0,
    ) + body


def recv_exact(connection: socket.socket, length: int) -> bytes:
    data = bytearray()
    while len(data) < length:
        chunk = connection.recv(length - len(data))
        if not chunk:
            raise AssertionError(f"连接提前关闭：{len(data)}/{length}")
        data.extend(chunk)
    return bytes(data)


def recv_frame(connection: socket.socket) -> tuple[tuple[int, ...], dict]:
    header = recv_exact(connection, HEADER_SIZE)
    fields = struct.unpack(">IHHHHIH", header)
    body = recv_exact(connection, fields[0] - HEADER_SIZE)
    return fields, json.loads(body.decode("utf-8"))


def send_json(connection: socket.socket, command: int, body: dict,
              pc_sequence: int, expected_mcu_sequence: int,
              response_command: int) -> dict:
    payload = json.dumps(body, ensure_ascii=False,
                         separators=(",", ":")).encode("utf-8")
    connection.sendall(make_frame(command, payload, pc_sequence))
    fields, response = recv_frame(connection)
    assert fields == (
        HEADER_SIZE + len(json.dumps(response, ensure_ascii=False,
                                     separators=(",", ":")).encode("utf-8")),
        VERSION, FLAG_JSON, 0, response_command,
        expected_mcu_sequence, 0,
    ), fields
    return response


def run(host: str, port: int) -> None:
    # 监测任务启动后先延时1秒发布OFF快照；这里给出少量调度余量。
    time.sleep(1.3)
    passed = 0

    with socket.create_connection((host, port), timeout=3.0) as connection:
        connection.settimeout(3.0)
        mcu_seq = 1
        pc_seq = 100

        body = send_json(connection, 0x0002, {}, pc_seq, mcu_seq, 0x0001)
        assert body == {}
        print("[PASS] 心跳使用ServiceID=0和MCU独立Seq=1")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        body = send_json(connection, 0x0106, {}, pc_seq, mcu_seq, 0x0006)
        assert body == {"requestCommandId": 0x0106, "status": "completed"}
        mcu_seq += 1; pc_seq += 1

        # 故意把PARA发到FUNC入口，验证串类拒绝，不能启动真实RK算法或电源。
        start_request = {
            "algorithmType": "parametric", "algorithmName": "idd_0", "algorithmIndex": 7,
            "power": {"memoryType": "LPDDR4X", "vdd1": "1800", "vdd2": "1100", "vddq": "600"},
            "reboot": True, "bootMode": "emmc", "TimingParameters": {},
        }
        body = send_json(connection, 0x0103, start_request, pc_seq, mcu_seq, 0x0006)
        assert body == {"requestCommandId": 259, "status": "failed", "errorCode": "notSupported", "algorithmIndex": 7}
        mcu_seq += 1; pc_seq += 1
        body = send_json(connection, 0x0103, {"algorithmIndex": 8}, pc_seq, mcu_seq, 0x0006)
        assert body == {"requestCommandId": 259, "status": "failed", "errorCode": "invalidArgument", "algorithmIndex": 8}
        mcu_seq += 1; pc_seq += 1
        connection.sendall(make_frame(0x0103, b'{"algorithmIndex":9,"algorithmIndex":10}', pc_seq))
        fields, body = recv_frame(connection)
        assert fields[3:6] == (0, 0x0006, mcu_seq)
        assert body == {"requestCommandId": 259, "status": "failed", "errorCode": "invalidArgument"}
        print("[PASS] 0103拒绝非FUNC、非法请求index回带和重复index拒绝；不执行RK")
        passed += 3
        mcu_seq += 1; pc_seq += 1

        body = send_json(connection, 0x0503, {}, pc_seq, mcu_seq, 0x0504)
        assert body == {"powerState": "off"}
        print("[PASS] 0x0503/0x0504读取OFF状态")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        body = send_json(connection, 0x0501, {}, pc_seq, mcu_seq, 0x0006)
        assert body["errorCode"] == "notConfigured"
        print("[PASS] 0x0501在未冻结时序时拒绝上电")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        body = send_json(connection, 0x0502, {}, pc_seq, mcu_seq, 0x0006)
        assert body == {"requestCommandId": 0x0502, "status": "completed"}
        print("[PASS] 0x0502在OFF状态幂等完成")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        body = send_json(connection, 0x0505, {}, pc_seq, mcu_seq, 0x0006)
        assert body["errorCode"] == "invalidState"
        print("[PASS] 0x0505拒绝重启已关闭的RK")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        body = send_json(connection, 0x0507, {}, pc_seq, mcu_seq, 0x0006)
        assert body["errorCode"] == "notConfigured"
        print("[PASS] 0x0507区分尚未成功设置Boot模式")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        body = send_json(connection, 0x0506, {"bootMode": "emmc"},
                         pc_seq, mcu_seq, 0x0006)
        assert body["errorCode"] == "executionFailed"
        print("[PASS] V2无I2C2/MCP4725时0506明确报告执行失败")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        body = send_json(connection, 0x0508, {}, pc_seq, mcu_seq, 0x0006)
        assert body["errorCode"] == "invalidDirection"
        print("[PASS] 0x0508反向发送返回invalidDirection")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        nominal = {
            "memoryType": "LPDDR4X",
            "vdd1": "1800", "vdd2": "1100", "vddq": "600",
        }
        body = send_json(connection, 0x0509, nominal,
                         pc_seq, mcu_seq, 0x0006)
        assert body == {"requestCommandId": 0x0509, "status": "completed"}
        print("[PASS] 0x0509 OFF+三轨标称电压选择LPDDR4X")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        adjusted = dict(nominal, vdd1="1810")
        body = send_json(connection, 0x0509, adjusted,
                         pc_seq, mcu_seq, 0x0006)
        assert body["status"] == "failed" and body["errorCode"] == "invalidState"
        print("[PASS] OFF状态拒绝缓存或提前应用非零拉偏")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        out_of_range = dict(nominal, vdd1="3360")
        body = send_json(connection, 0x0509, out_of_range,
                         pc_seq, mcu_seq, 0x0006)
        assert body["errorCode"] == "invalidArgument"
        print("[PASS] 0x0509拒绝原生档位越界")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        body = send_json(connection, 0x0601, {}, pc_seq, mcu_seq, 0x0602)
        assert body["samplePowerState"] == "off" and body["validMask"] == 0
        assert all(name not in body for name in ("voltageMv", "currentMa", "powerMw"))
        print("[PASS] 0x0601返回OFF缓存快照且不伪造0V/0mA")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        # 070x彻底删除：包括旧响应方向也统一按未知命令拒绝，不更新结果灯。
        for command in (0x0701, 0x0702, 0x0703):
            body = send_json(connection, command, {}, pc_seq, mcu_seq, 0x0006)
            assert body == {"requestCommandId": command, "status": "failed", "errorCode": "notSupported"}
            mcu_seq += 1; pc_seq += 1
        print("[PASS] 070x旧入口全部notSupported")
        passed += 3

        body = send_json(connection, 0x0107,
                         {"failChips": [{"physicalAddr": "", "mirrorIndex": 3}]},
                         pc_seq, mcu_seq, 0x0006)
        assert body["errorCode"] == "invalidArgument"
        mcu_seq += 1; pc_seq += 1
        body = send_json(connection, 0x0107, {"failChips": []}, pc_seq, mcu_seq, 0x0006)
        assert body == {"requestCommandId": 0x0107, "status": "completed"}
        print("[PASS] 0107拒绝未知镜像且整包不改灯，空失败表收尾；三灯保留PASS")
        passed += 2

    with socket.create_connection((host, port), timeout=3.0) as connection:
        connection.settimeout(3.0)
        body = send_json(connection, 0x0002, {}, 900, 1, 0x0001)
        assert body == {}
    print("[PASS] 新连接MCU发送Seq重新从1开始")
    passed += 1

    with socket.create_connection((host, port), timeout=3.0) as connection:
        connection.settimeout(3.0)
        # 非0 ServiceID前缀被丢弃，后面的完整心跳须在同一连接正常响应。
        connection.sendall(make_frame(0x0002, b"{}", 901, service=1) +
                           make_frame(0x0002, b"{}", 902))
        fields, body = recv_frame(connection)
        assert fields[3:6] == (0, 0x0001, 1) and body == {}
        nul_body = br'{"failChips":[{"physicalAddr":"bad\u0000tail","mirrorIndex":0}]}'
        connection.sendall(make_frame(0x0107, nul_body, 903))
        fields, body = recv_frame(connection)
        assert fields[3:6] == (0, 0x0006, 2)
        assert body["errorCode"] == "invalidArgument"
        assert send_json(connection, 0x0002, {}, 904, 3, 0x0001) == {}
    print("[PASS] 非法前缀与JSON NUL拒绝后，同连接心跳继续")
    passed += 2

    print(f"PC business network: {passed} PASS, 0 FAIL")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="192.168.1.201")
    parser.add_argument("--port", type=int, default=8888)
    args = parser.parse_args()
    run(args.host, args.port)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
