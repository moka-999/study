#!/usr/bin/env python3
"""独立网络升级工具公共实现：只用Python 3.10+标准库，无需工程、编译器或ST-Link。

仅支持已部署完整OTA的板卡，固定使用mcu_*动作；不会关电、清busy或强制重启。
未提交的传输中断不续传；finish发送后即使应答丢失，也只重连查询、不重复提交。
"""
from __future__ import annotations

import argparse
import base64
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import socket
import struct
import sys
import time
import zlib

BOARD_NAMES = {"b1-h743": "B1", "apollo-v2-h743": "V2"}
IMAGE_LIMIT = 896 * 1024
PACKAGE_LIMIT = 2 * 1024 * 1024
HEADER = struct.Struct(">IHHHHIH")
A1_BASE, A2_BASE = 0x08020000, 0x08100000


class ProtocolError(ValueError):
    """对端响应不符合现有合同；禁止据此继续写入或报告成功。"""


class DeviceError(RuntimeError):
    """保留MCU原始错误码，便于区分busy、未就绪及提交状态不确定。"""

    def __init__(self, body: dict):
        self.code = body.get("errorCode", "unknown")
        super().__init__(f"MCU拒绝：{self.code}（{json.dumps(body, ensure_ascii=False)}）")


def require(condition: bool, message: str) -> None:
    """外部输入校验不用assert，Python以-O运行时也不能跳过。"""
    if not condition:
        raise ProtocolError(message)


def parse_version(text: str) -> tuple[int, ...]:
    """三段0..65535整数；用于比较包版本与W25查询所得运行版本。"""
    require(isinstance(text, str) and re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", text),
            "版本号必须为三段非负整数，例如0.1.37")
    result = tuple(map(int, text.split(".")))
    require(all(value <= 65535 for value in result), "版本号每段不能超过65535")
    return result


@dataclass(frozen=True)
class Package:
    data: bytes
    version: tuple[int, ...]
    package_id: str
    image_bytes: int
    target: str

    def describe(self) -> dict:
        """日志只记录摘要，不记录完整固件或Base64正文。"""
        return {"target": self.target, "version": ".".join(map(str, self.version)),
                "packageId": self.package_id, "packageBytes": len(self.data),
                "imageBytes": self.image_bytes, "sha256": hashlib.sha256(self.data).hexdigest()}


def validate_package(data: bytes, target: str) -> Package:
    """只读核对现有OTA1格式；复算A1/A2及各区CRC，不生成或修改固件。

    修正项允许非4字节对齐，但必须递增、不重叠，值只能指向A1镜像。
    这些检查防止误选/损坏文件，不是签名认证，不能替代MCU和Boot的二次校验。
    """
    require(target in BOARD_NAMES, "升级入口板型非法")
    require(120 <= len(data) <= PACKAGE_LIMIT, "升级包长度非法；请选择.ota，不是bin或axf")
    header = struct.unpack_from("<IHHHH7I", data)
    require(header[:5] == (0x3141544F, 1, 40, 1, 0) and header[10] == 0,
            "不是支持的OTA1文件格式")
    require(header[5] == len(data) and header[11] == zlib.crc32(data[:36]), "包长度/包头CRC错误")
    version = data[40:116]
    require(struct.unpack_from("<2H", version) == (1, 76) and version[10:12] == b"\0\0",
            "版本信息结构错误")
    require(zlib.crc32(version) == header[9], "版本信息CRC错误")
    for start, size in ((20, 24), (44, 16), (60, 16)):
        head, separator, tail = version[start:start + size].partition(b"\0")
        require(bool(head and separator) and not any(tail), "版本字符串缺少结束符或填充非法")
        head.decode("utf-8", errors="strict")
    require(version[60:76] == target.encode().ljust(16, b"\0"),
            f"板型不符：{BOARD_NAMES[target]}入口只接受{target}，拒绝另一板型的包")
    image_bytes, image_crc = struct.unpack_from("<2I", version, 12)
    count = header[6]
    require(8 <= image_bytes <= IMAGE_LIMIT and 1 <= count <= image_bytes // 4,
            "App超过896KiB或地址修正表长度非法")
    boundary = 116 + count * 4
    require(boundary + image_bytes == len(data), "修正表/App边界错误或文件有多余尾数据")
    table, image = data[116:boundary], data[boundary:]
    require(zlib.crc32(table) == header[7] and zlib.crc32(image) == image_crc, "修正表或A1镜像CRC错误")
    moved = bytearray(image)
    previous = 0
    for index, (offset,) in enumerate(struct.iter_unpack("<I", table)):
        require(previous + 4 <= offset <= image_bytes - 4 and (index != 0 or offset == 4),
                "修正表重复/乱序/越界，或缺少复位向量修正")
        value = struct.unpack_from("<I", image, offset)[0]
        require(A1_BASE <= value <= A1_BASE + image_bytes, "修正项指向镜像外，不能修改RAM/外设地址")
        struct.pack_into("<I", moved, offset, value + A2_BASE - A1_BASE)
        previous = offset
    require(zlib.crc32(moved) == header[8], "A2修正镜像CRC错误")
    sp, entry = struct.unpack_from("<2I", image)
    ram = ((0x20000000, 0x20020000), (0x24000000, 0x24080000),
           (0x30000000, 0x30048000), (0x38000000, 0x38010000))
    require(sp % 8 == 0 and any(first < sp <= last for first, last in ram)
            and entry & 1 and A1_BASE + 8 <= (entry & ~1) <= A1_BASE + image_bytes - 2,
            "App初始栈或复位入口非法")
    return Package(data, struct.unpack_from("<3H", version, 4), f"{header[11]:08X}", image_bytes, target)


def read_package(path: Path, target: str) -> Package:
    require(path.suffix.lower() == ".ota", "请选择.ota文件，不能直接发送bin/axf/elf")
    with path.open("rb") as source:
        return validate_package(source.read(PACKAGE_LIMIT + 1), target)


def json_object(pairs: list) -> dict:
    """拒绝重复键，避免成功状态或包标识被同名字段覆盖。"""
    result = {}
    for key, value in pairs:
        require(key not in result, f"响应JSON含重复字段：{key}")
        result[key] = value
    return result


class Client:
    """单连接顺序请求；PC/MCU Seq独立，不要求MCU回显请求流水号。"""

    def __init__(self, connection, log, timeout=30):
        self.connection, self.log = connection, log
        self.tx, self.rx = 1, None
        self.timeout, self.deadline = timeout, 0.0

    def recv_exact(self, count: int) -> bytes:
        data = bytearray()
        while len(data) < count:
            remaining = self.deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("响应整帧接收超时")
            self.connection.settimeout(remaining)
            block = self.connection.recv(count - len(data))
            if not block:
                raise ConnectionError("对端已断开连接")
            data.extend(block)
        return bytes(data)

    def call(self, body: dict, *, command=0x0901, expected=(0x0902,)) -> tuple[int, dict]:
        payload = json.dumps(body, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")
        length = HEADER.size + len(payload)
        require(length <= 4096, "完整PDU超过4096字节，未发送")
        self.log({"event": "send", "command": f"{command:04X}", "seq": self.tx, "bytes": length,
                  "body": {key: value for key, value in body.items() if key != "data"}})
        self.connection.settimeout(self.timeout)
        self.connection.sendall(HEADER.pack(length, 1, 8, 0, command, self.tx, 0) + payload)
        self.tx = (self.tx + 1) & 0xFFFFFFFF
        self.deadline = time.monotonic() + self.timeout
        header = HEADER.unpack(self.recv_exact(HEADER.size))
        require(18 <= header[0] <= 4096 and header[1:4] == (1, 8, 0) and header[6] == 0,
                "响应帧头非法")
        require(self.rx is None or header[5] == self.rx, "MCU响应Seq不连续；不猜测或跳过响应")
        self.rx = (header[5] + 1) & 0xFFFFFFFF
        raw = self.recv_exact(header[0] - 18).decode("utf-8", errors="strict")
        response = json.loads(raw, object_pairs_hook=json_object,
                              parse_constant=lambda value: require(False, f"JSON非法数字：{value}"))
        require(isinstance(response, dict), "响应正文必须为JSON对象")
        self.log({"event": "receive", "command": f"{header[4]:04X}", "seq": header[5], "body": response})
        if header[4] == 0x0006:
            require(response.get("requestCommandId") == command and response.get("status") == "failed",
                    "收到无关或格式不符的通用应答")
            raise DeviceError(response)
        require(header[4] in expected, f"响应命令不符：{header[4]:04X}")
        if command == 0x0901:
            require(response.get("action") == body.get("action"), "OTA动作不匹配")
            if "packageId" in body:
                require(response.get("packageId") == body["packageId"], "OTA包标识不匹配")
        return header[4], response


@contextmanager
def connected(host: str, port: int, log, timeout=30):
    """每次只持有一个连接；退出必关闭，不抢占或关闭别人的客户端。"""
    with socket.create_connection((host, port), timeout=min(3, timeout)) as connection:
        connection.settimeout(timeout)
        yield Client(connection, log, timeout)


def query(client: Client) -> tuple[dict, dict]:
    """只查询W25固件/健康状态及0504逻辑电源状态，不读写FRU或操作电源。"""
    _, status = client.call({"action": "mcu_query"})
    require(status.get("status") == "completed", "固件查询未完成")
    parse_version(status.get("firmwareVersion"))
    _, power = client.call({}, command=0x0503, expected=(0x0504,))
    require(power.get("powerState") in ("off", "on", "starting", "stopping", "fault"), "电源状态缺失或非法")
    print(f"运行版本：{status['firmwareVersion']}；健康确认：{status.get('confirmed')}；"
          f"OTA状态：{status.get('state')}；RK/DDR逻辑电源：{power['powerState']}", flush=True)
    return status, power


def wait_confirmed(host: str, port: int, package: Package, log, wait_seconds=180) -> dict:
    """finish之后只做有界的只读重连，不重发begin/write/finish或执行强制复位。"""
    deadline = time.monotonic() + wait_seconds
    while time.monotonic() < deadline:
        time.sleep(min(2, max(0, deadline - time.monotonic())))
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            with connected(host, port, log, timeout=min(10, remaining)) as client:
                command, status = client.call({"action": "mcu_query", "packageId": package.package_id},
                                              expected=(0x0902, 0x0903))
                require(status.get("status") == "completed", "升级后查询未完成")
                if command == 0x0903:
                    require(status.get("confirmed") is True and status.get("state") == "confirmed"
                            and parse_version(status.get("firmwareVersion")) == package.version,
                            "0903未同时满足目标版本及健康确认，不认定升级成功")
                    log({"event": "confirmed", "body": status})
                    print("升级成功：目标包、运行版本和健康确认均匹配。", flush=True)
                    return status
        except OSError as error:
            log({"event": "reconnect", "error": str(error)})
    raise TimeoutError("未在观察期内确认目标包；可能仍在重启、失联或已回滚。只查询核实，勿直接重复升级。")


def upgrade(host: str, port: int, package: Package, log) -> dict:
    """健康+OFF预检后顺序上传；busy等最终门禁仍由MCU原子判断。

    0504是软件状态，不是实际电压证明；操作者必须先独立停止RK测试及B2加热。
    此工具没有任何关电/复位/取消/清busy路径。
    """
    with connected(host, port, log) as client:
        status, power = query(client)
        log({"event": "before", "firmware": status, "power": power})
        require(status.get("confirmed") is True and status.get("state") in ("confirmed", "cancelled", "failed")
                and "download" not in status, "当前App未健康确认或已有升级事务；本工具不接管旧事务")
        require(power["powerState"] == "off", "请先按正常流程停止测试及关闭RK/DDR电源；工具不会自动关电")
        require(package.version > parse_version(status["firmwareVersion"]), "包版本必须高于板上版本，不强制同版重刷或降级")
        _, answer = client.call({"action": "mcu_begin", "packageId": package.package_id, "packageSize": len(package.data)})
        size = answer.get("blockSize")
        require(answer.get("status") == "completed" and answer.get("state") == "receiving"
                and type(answer.get("nextOffset")) is int and answer["nextOffset"] == 0
                and type(size) is int and 1 <= size <= 3000, "begin应答非法；不续传旧事务")
        for offset in range(0, len(package.data), size):
            block = package.data[offset:offset + size]
            _, answer = client.call({"action": "mcu_write", "packageId": package.package_id,
                                     "offset": offset, "data": base64.b64encode(block).decode("ascii")})
            require(answer.get("status") == "completed" and answer.get("state") == "receiving"
                    and type(answer.get("offset")) is int and answer["offset"] == offset
                    and type(answer.get("nextOffset")) is int and answer["nextOffset"] == offset + len(block),
                    "write确认偏移不符；停止传输，不盲目补发")
            print(f"\r已传输 {offset + len(block)}/{len(package.data)} 字节", end="", flush=True)
        print("\n提交升级，随后只读等待Boot安装及健康确认……", flush=True)
        log({"event": "finish_attempt", "packageId": package.package_id})
        try:
            _, answer = client.call({"action": "mcu_finish", "packageId": package.package_id})
            require(answer.get("status") == "accepted" and answer.get("state") == "ready", "finish应答非法")
        except DeviceError as error:
            if error.code != "commitUnknown":
                raise
            log({"event": "finish_unknown", "error": str(error)})
        except (OSError, ValueError) as error:
            # 请求可能已经提交；只读确认比再次发送finish安全。
            log({"event": "finish_unknown", "error": str(error)})
    return wait_confirmed(host, port, package, log)


def main(argv=None, *, target: str, tool_dir: Path) -> int:
    """两个入口固定各自板型；线上不新增板型字段，也不允许CLI覆盖入口板型。"""
    require(target in BOARD_NAMES, "升级入口板型非法")
    board = BOARD_NAMES[target]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", nargs="?", choices=("query", "upgrade", "inspect"))
    parser.add_argument("--host", help="明确指定MCU地址；不会扫描网络或假定生产IP")
    parser.add_argument("--port", type=int, default=8888)
    parser.add_argument("--package", type=Path, help=f"已经打好的{board} .ota文件")
    parser.add_argument("--confirm-idle", action="store_true", help="确认RK测试/B2加热停止、PC断开且允许MCU重启")
    parser.add_argument("--log-dir", type=Path, default=tool_dir / "logs")
    args = parser.parse_args(argv)
    if args.action is None:
        print(f"{board}独立升级：1=只读查询，2=网络升级，3=离线检查升级包，其他=退出")
        args.action = {"1": "query", "2": "upgrade", "3": "inspect"}.get(input("请选择：").strip())
        if args.action is None:
            return 0
        if args.action != "inspect":
            args.host = input(f"{board} IP地址（不含端口）：").strip()
        if args.action != "query":
            args.package = Path(input(".ota完整路径（可拖入窗口）：").strip().strip('"'))
        if args.action == "upgrade":
            print(f"请核对IP属于{board}，已停止RK测试和B2加热，RK/DDR已OFF，PC/网络助手已断开。")
            print("升级会重启MCU；查询不提供硬件板型，不能凭测试仪名称判断板型。")
            args.confirm_idle = input("确认以上条件并允许升级，请输入 YES：").strip() == "YES"
    if not 1 <= args.port <= 65535:
        parser.error("端口须为1..65535")
    if args.action != "inspect" and (not args.host or any(char.isspace() for char in args.host)):
        parser.error("请明确填写MCU地址 --host；不使用隐含默认IP")
    if args.action != "query" and args.package is None:
        parser.error("检查/升级必须指定 --package")
    if args.action == "upgrade" and not args.confirm_idle:
        parser.error("未确认空闲，不连接设备；请核实条件后使用 --confirm-idle")
    args.log_dir.mkdir(parents=True, exist_ok=True)
    log_path = args.log_dir / (datetime.now().strftime("%Y%m%d-%H%M%S-%f") + ".jsonl")
    print(f"本轮日志：{log_path}", flush=True)
    with log_path.open("x", encoding="utf-8") as output:
        def log(event):
            output.write(json.dumps({"time": datetime.now().isoformat(timespec="milliseconds"), **event},
                                    ensure_ascii=False, allow_nan=False) + "\n")
            output.flush()

        log({"event": "start", "action": args.action, "host": args.host, "port": args.port, "target": target})
        try:
            package = None
            if args.action != "query":
                package = read_package(args.package, target)
                log({"event": "package", "path": str(args.package.resolve()), **package.describe()})
                print(json.dumps(package.describe(), ensure_ascii=False, indent=2), flush=True)
            if args.action == "inspect":
                print("离线文件校验通过；未连接设备。")
            elif args.action == "query":
                with connected(args.host, args.port, log) as client:
                    status, power = query(client)
                    log({"event": "query", "firmware": status, "power": power})
            else:
                upgrade(args.host, args.port, package, log)
            log({"event": "done"})
            return 0
        except (Exception, KeyboardInterrupt) as error:
            log({"event": "error", "error": repr(error)})
            print(f"未确认成功：{error}\n连接已关闭；未自动关电、清busy或重发升级。请保留日志。", file=sys.stderr)
            return 1


if __name__ == "__main__":
    raise SystemExit("请运行v2_ota.py或b1_ota.py固定板型入口，不直接执行公共实现。")
