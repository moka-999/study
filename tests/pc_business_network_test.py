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


def expect_disconnect(host: str, port: int, payload: bytes) -> None:
    with socket.create_connection((host, port), timeout=3.0) as connection:
        connection.settimeout(2.0)
        connection.sendall(payload)
        try:
            assert connection.recv(1) == b""
        except (ConnectionResetError, ConnectionAbortedError):
            return


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
            "vdd1Steps": 0, "vdd2Steps": 0, "vddqSteps": 0,
        }
        body = send_json(connection, 0x0509, nominal,
                         pc_seq, mcu_seq, 0x0006)
        assert body == {"requestCommandId": 0x0509, "status": "completed"}
        print("[PASS] 0x0509 OFF+全零选择LPDDR4X")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        adjusted = dict(nominal, vdd1Steps=1)
        body = send_json(connection, 0x0509, adjusted,
                         pc_seq, mcu_seq, 0x0006)
        assert body["status"] == "failed" and body["errorCode"] == "invalidState"
        print("[PASS] OFF状态拒绝缓存或提前应用非零拉偏")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        out_of_range = dict(nominal, vdd1Steps=78)
        body = send_json(connection, 0x0509, out_of_range,
                         pc_seq, mcu_seq, 0x0006)
        assert body["errorCode"] == "invalidArgument"
        print("[PASS] 0x0509拒绝20mV档位越界")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        body = send_json(connection, 0x0601, {}, pc_seq, mcu_seq, 0x0602)
        assert body["samplePowerState"] == "off" and body["validMask"] == 0
        assert all(name not in body for name in ("voltageMv", "currentMa", "powerMw"))
        print("[PASS] 0x0601返回OFF缓存快照且不伪造0V/0mA")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        for target, state in (
            ("lpddr0", "pass"),
            ("lpddr1", "fail"),
            ("emcp", "testHot"),
        ):
            body = send_json(connection, 0x0701,
                             {"target": target, "state": state},
                             pc_seq, mcu_seq, 0x0006)
            assert body == {"requestCommandId": 0x0701, "status": "completed"}
            mcu_seq += 1; pc_seq += 1

        body = send_json(connection, 0x0702, {}, pc_seq, mcu_seq, 0x0703)
        assert body == {
            "lpddr0": "pass", "lpddr1": "fail", "emcp": "testHot"
        }
        print("[PASS] 0x0701设置三对象，0x0702/0x0703原子查询")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        body = send_json(connection, 0x0701,
                         {"target": "lpddr0", "state": "pass", "extra": 1},
                         pc_seq, mcu_seq, 0x0006)
        assert body["errorCode"] == "invalidArgument"
        print("[PASS] 0x0701拒绝多余字段")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        body = send_json(connection, 0x0703, {}, pc_seq, mcu_seq, 0x0006)
        assert body["errorCode"] == "invalidDirection"
        print("[PASS] MCU方向响应命令反向发送返回invalidDirection")
        passed += 1
        mcu_seq += 1; pc_seq += 1

        # 测试结束恢复三组全灭，避免把测试显示留在开发板替代引脚上。
        for target in ("lpddr0", "lpddr1", "emcp"):
            body = send_json(connection, 0x0701,
                             {"target": target, "state": "off"},
                             pc_seq, mcu_seq, 0x0006)
            assert body["status"] == "completed"
            mcu_seq += 1; pc_seq += 1

    with socket.create_connection((host, port), timeout=3.0) as connection:
        connection.settimeout(3.0)
        body = send_json(connection, 0x0002, {}, 900, 1, 0x0001)
        assert body == {}
    print("[PASS] 新连接MCU发送Seq重新从1开始")
    passed += 1

    expect_disconnect(host, port, make_frame(0x0002, b"{}", 901, service=1))
    print("[PASS] 非0 ServiceID关闭连接")
    passed += 1

    nul_body = br'{"target":"lpddr0","state":"pass\u0000junk"}'
    expect_disconnect(host, port, make_frame(0x0701, nul_body, 902))
    print("[PASS] JSON字符串\\u0000绕过被统一入口拒绝")
    passed += 1

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
