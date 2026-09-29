#!/usr/bin/env python3
"""真实TCP OTA验收。默认只查询；--exercise写下载区，--upgrade提交并重启开发板。

默认使用mcu_*动作；仅升级旧固件时加--legacy-ota-actions，不自动降级写操作。
"""

import argparse
import base64
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import socket
import struct
import time
import zlib

from pc_business_network_test import make_frame, recv_exact


MCU_ACTIONS = ("mcu_begin", "mcu_write", "mcu_finish", "mcu_query", "mcu_cancel")


class Client:
    """复用正式18字节帧；两端序号分别递增，不用请求Seq匹配业务应答。"""

    def __init__(self, connection, events, *, legacy_ota_actions=False):
        self.connection, self.events = connection, events
        self.tx, self.rx = 100, 1
        # 旧版兼容必须由操作者明确启用；发送失败不会自动换动作重试写入。
        self.legacy_ota_actions = legacy_ota_actions

    def call(self, body, *, command=0x0901, expect=0x0902, error=None, split=False,
             pdu_bytes=None, heartbeat=False):
        if command == 0x0901 and self.legacy_ota_actions and body.get("action") in MCU_ACTIONS:
            body = dict(body, action=body["action"][4:])
        payload = json.dumps(body, separators=(",", ":")).encode()
        if pdu_bytes is not None:
            assert 18 + len(payload) <= pdu_bytes <= 4096
            payload += b" " * (pdu_bytes - 18 - len(payload))
        wire = make_frame(command, payload, self.tx)
        assert len(wire) <= 4096
        request_bytes = len(wire)
        if heartbeat:
            wire += make_frame(0x0002, b"{}", self.tx + 1)
        started = time.monotonic()
        if split:
            for first, last in ((0, 3), (3, 18), (18, 97), (97, len(wire))):
                if first < len(wire):
                    self.connection.sendall(wire[first:last])
                    time.sleep(0.015)
        else:
            self.connection.sendall(wire)
        result = self.receive(body, command, expect, error)
        self.events[-1].update(requestBytes=request_bytes,
                               elapsedMs=round((time.monotonic() - started) * 1000, 3))
        if "data" in body:
            self.events[-1]["base64Chars"] = len(body["data"])
        if heartbeat:
            self.receive({}, 0x0002, 0x0001, None)
        return result

    def receive(self, body, command, expect, error):
        """按帧接收独立递增响应；粘包中的心跳也必须正常返回。"""
        header = struct.unpack(">IHHHHIH", recv_exact(self.connection, 18))
        assert 18 <= header[0] <= 4096, header
        response = json.loads(recv_exact(self.connection, header[0] - 18))
        self.events.append({"command": command, "request": {k: v for k, v in body.items() if k != "data"},
                            "responseCommand": header[4], "mcuSequence": header[5], "response": response})
        assert header[1:4] == (1, 8, 0) and header[5:] == (self.rx, 0), header
        self.tx += 1
        self.rx += 1
        if expect is not None:
            assert header[4] == expect, response
        if error:
            assert response["status"] == "failed" and response["errorCode"] == error, response
        if command == 0x0901 and header[4] in (0x0902, 0x0903):
            assert response["action"] == body["action"], response
        return header[4], response

    def query_after_upgrade(self, package_id, *, allow_legacy=False):
        """重连先查新动作；只有显式旧版迁移且不支持新动作时允许只读回退。"""
        assert not self.legacy_ota_actions, "升级后查询须先使用新动作"
        body = {"action": "mcu_query", "packageId": package_id}
        command, response = self.call(body, expect=None)
        if (allow_legacy and command == 0x0006 and response.get("status") == "failed"
                and response.get("errorCode") == "notSupported"):
            return self.call(dict(body, action="query"), expect=None)
        return command, response


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.1.201")
    parser.add_argument("--port", type=int, default=8888)
    parser.add_argument("--package", type=Path)
    parser.add_argument("--exercise", action="store_true", help="写下载区并验证拒绝/取消/超时，不提交升级")
    parser.add_argument("--upgrade", action="store_true", help="传完整包、提交并等待重启后0903")
    parser.add_argument("--legacy-ota-actions", action="store_true",
                        help="仅旧版迁移：上传阶段用无mcu_前缀动作；重启后先查新动作，不自动重试写入")
    parser.add_argument("--expect-block-size", type=int, help="核对本次固件公布的单块上限；按mcu_begin协商，兼容旧版1024")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error("报告已存在，请换文件名保留原证据")
    report = {"host": args.host, "started": time.strftime("%Y-%m-%d %H:%M:%S"),
              "legacyOtaActions": args.legacy_ota_actions, "events": [], "passed": []}

    @contextmanager
    def connected(*, after_upgrade=False):
        with socket.create_connection((args.host, args.port), timeout=3) as sock:
            sock.settimeout(30)
            yield Client(sock, report["events"],
                         legacy_ota_actions=args.legacy_ota_actions and not after_upgrade)
        time.sleep(0.2)  # 单客户端服务器需先收尾旧连接。

    def passed(name):
        report["passed"].append(name)
        print("PASS:", name, flush=True)

    try:
        with connected() as c:
            _, status = c.call({"action": "mcu_query"})
            report["before"] = status
            print(json.dumps(status, ensure_ascii=False), flush=True)
            passed("从W25查询当前版本和健康状态")
        if not (args.exercise or args.upgrade):
            return
        assert status["confirmed"] and status["state"] == "confirmed", status
        if not args.package:
            raise ValueError("写测试必须提供已验证的.ota包")
        package = args.package.read_bytes()
        assert package[:4] == b"OTA1" and len(package) == struct.unpack_from("<I", package, 12)[0]
        identity = struct.unpack_from("<I", package, 36)[0]
        assert identity == zlib.crc32(package[:36])
        package_id = f"{identity:08X}"
        version = ".".join(map(str, struct.unpack_from("<3H", package, 44)))
        report.update(package=str(args.package.resolve()), packageId=package_id,
                      package_sha256=hashlib.sha256(package).hexdigest(), targetVersion=version)
        begin = {"action": "mcu_begin", "packageId": package_id, "packageSize": len(package)}

        def block(offset, raw):
            return {"action": "mcu_write", "packageId": package_id, "offset": offset,
                    "data": base64.b64encode(raw).decode()}

        def upload(c, *, corrupt=False):
            _, response = c.call(begin)
            assert response["nextOffset"] == 0
            size = block_size(response)
            for offset in range(0, len(package), size):
                raw = package[offset:offset + size]
                if corrupt and offset + len(raw) == len(package):
                    raw = raw[:-1] + bytes([raw[-1] ^ 1])
                _, response = c.call(block(offset, raw), split=offset == 0, heartbeat=offset == size)
                assert response["offset"] == offset and response["nextOffset"] == offset + len(raw), response
                if offset == 0:
                    _, repeat = c.call(block(offset, raw))
                    assert repeat["nextOffset"] == len(raw)
                if offset == 0 or offset // 32768 != (offset + len(raw)) // 32768 or offset + len(raw) == len(package):
                    print(f"传输 {offset + len(raw)}/{len(package)} 字节", flush=True)

        def block_size(response):
            size = response["blockSize"]
            assert type(size) is int and 1 <= size <= 3000, response
            if args.expect_block_size is not None:
                assert size == args.expect_block_size, response
            report["blockSize"] = size
            return size

        if args.exercise:
            with connected() as c:
                c.call({}, command=0x0902, expect=6, error="invalidDirection")
                c.call({}, command=0x0903, expect=6, error="invalidDirection")
                c.call({"action": "unknown"}, expect=6, error="notSupported")
                c.call(dict(begin, packageSize=True), expect=6, error="invalidArgument")
                c.call(dict(begin, extra=0), expect=6, error="invalidArgument")
                c.call(dict(begin, packageId="aabbccdd"), expect=6, error="invalidArgument")
                _, response = c.call(begin)
                size = block_size(response)
                c.call(begin)
                for data in ("", "Zh==", "Zm9=", "Zg==AAAA", "====", "AA_A", "AA A"):
                    c.call(dict(block(0, b"x"), data=data), expect=6, error="invalidArgument")
                c.call(block(1, package[:4]), expect=6, error="offsetMismatch")
                c.call(block(0, package[:size + 1]), expect=6, error="invalidArgument")
                c.call(block(0, package[:size]), split=True, pdu_bytes=4096)
                c.call(block(0, package[:size]), heartbeat=True)
                c.call(block(0, bytes([package[0] ^ 1]) + package[1:size]), expect=6, error="dataMismatch")
                c.call({"action": "mcu_finish", "packageId": package_id}, expect=6, error="invalidState")
                _, response = c.call({"action": "mcu_query"})
                assert response["download"]["receivedBytes"] == size
                c.call({}, command=0x0501, expect=6, error="otaNotReady")
                c.call({}, command=0x0002, expect=1)
                c.call({"action": "mcu_cancel", "packageId": package_id})
                c.call({"action": "mcu_cancel", "packageId": package_id})
                passed(f"字段/Base64/{size + 1}超限拒绝/4096整帧半包/粘包/重复块/取消/维护门/心跳")
                c.call(begin)
                c.call(block(0, package[:64]))
            with connected() as c:
                _, response = c.call(begin)
                assert response["nextOffset"] == 0
                passed("未提交断线后从0开始")
                for _ in range(16):
                    time.sleep(2)
                    c.call({}, command=0x0002, expect=1)
                _, response = c.call({"action": "mcu_query"})
                assert response["state"] == "cancelled" and "download" not in response
                passed("30秒下载空闲自动取消，心跳不续期")
                upload(c, corrupt=True)
                c.call({"action": "mcu_finish", "packageId": package_id}, expect=6, error="crcMismatch")
                _, response = c.call({"action": "mcu_query"})
                assert response["state"] == "failed" and response["firmwareVersion"] == status["firmwareVersion"]
                passed("完整坏包CRC拒绝，仍运行原版本")
            # 只发送声明4097字节的头，状态机必须立即断开，不等待正文或访问Flash。
            with connected() as c:
                c.connection.sendall(struct.pack(">IHHHHIH", 4097, 1, 8, 0, 0x0901, 100, 0))
                try:
                    assert c.connection.recv(1) == b"", "超长帧未断开"
                except ConnectionResetError:
                    pass
            with connected() as c:
                c.call({}, command=0x0002, expect=1)
                passed("4097字节PDU立即拒绝，重连心跳恢复")
        if args.upgrade:
            with connected() as c:
                upload(c)
                _, response = c.call({"action": "mcu_finish", "packageId": package_id})
                assert response["status"] == "accepted" and response["state"] == "ready"
                # 提交后重复finish仍只接受一次；cancel不能撤销已落盘的启动请求。
                c.call({"action": "mcu_finish", "packageId": package_id})
                c.call({"action": "mcu_cancel", "packageId": package_id}, expect=6, error="invalidState")
                passed("TCP完整文件提交READY；重复finish幂等")
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                time.sleep(2)
                try:
                    with connected(after_upgrade=True) as c:
                        command, response = c.query_after_upgrade(package_id, allow_legacy=args.legacy_ota_actions)
                        report["after"] = response
                        if command == 0x0903:
                            assert response["confirmed"] and response["firmwareVersion"] == version
                            assert c.query_after_upgrade(package_id, allow_legacy=args.legacy_ota_actions)[0] == 0x0903
                            c.call({}, command=0x0002, expect=1)
                            passed("重启安装目标版本，30秒确认后查询0903，心跳正常")
                            break
                except (OSError, AssertionError) as error:
                    report["reconnect"] = str(error)
            else:
                raise AssertionError("等待目标包健康确认超时；不自动重发finish或强制降版")
    except BaseException as error:
        report["error"] = repr(error)
        raise
    finally:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
