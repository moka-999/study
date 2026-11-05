"""B1验板工具：现有TCP协议、人工电压清单和无SPL上下电循环。"""
from __future__ import annotations

import csv
import json
import math
import socket
import struct
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

HEADER = struct.Struct(">IHHHHIH")
RAILS = ("VDD1", "VDD2", "VDDQ")
LIMITS = ((800, 3350, 10), (400, 1675, 5), (400, 1675, 5))
BOOT_MV = {"usbMaskrom": 0, "sdCard": 300, "emmc": 600, "fspiM0": 900,
           "fspiM1": 1200, "fspiM2": 1500, "emmcSd": 1800}
ALLOWED_COMMANDS = {0x0002, 0x0101, 0x0501, 0x0502, 0x0503,
                    0x0506, 0x0507, 0x0509, 0x0601}


class RemoteError(RuntimeError):
    """MCU明确拒绝或执行失败；不自动重试。"""


class Stopped(Exception):
    """用户停止后，不再提交下一条操作；已发送命令仍收完应答。"""


def nominal(memory_type):
    if memory_type not in ("LPDDR4", "LPDDR4X"):
        raise ValueError("内存类型只能是LPDDR4或LPDDR4X")
    return (1800, 1100, 1100 if memory_type == "LPDDR4" else 600)


def finite_number(value):
    return type(value) in (int, float) and math.isfinite(value)


@dataclass(frozen=True)
class VoltagePoint:
    mv: tuple[int, int, int]
    reference_mv: tuple = (None, None, None)
    reference_ma: tuple = (None, None, None)

    def __post_init__(self):
        if len(self.mv) != 3:
            raise ValueError("必须提供三路电压")
        for rail, value, (low, high, step) in zip(RAILS, self.mv, LIMITS):
            if type(value) is not int or not low <= value <= high or (value - low) % step:
                raise ValueError(f"{rail}必须为{low}～{high}mV，步进{step}mV，不自动取整")
        for values in (self.reference_mv, self.reference_ma):
            if len(values) != 3 or any(v is not None and
                                      (not finite_number(v) or v < 0) for v in values):
                raise ValueError("参考值须为非负有限数字，未知请留空")

    def payload(self, memory_type):
        nominal(memory_type)
        return dict(zip(("memoryType", "vdd1", "vdd2", "vddq"),
                        (memory_type, *(str(v) for v in self.mv))))


class Client:
    """单连接、单在途请求；序号双向独立，业务以响应命令及requestCommandId匹配。"""

    def __init__(self, event=lambda _: None):
        self.sock = None
        self.tx = 1
        self.last_rx = None
        self.buffer = bytearray()
        self.event = event

    def connect(self, host, port):
        self.close()
        self.sock = socket.create_connection((host, port), timeout=3)
        self.sock.settimeout(0.2)
        self.tx, self.last_rx = 1, None
        self.buffer.clear()

    def close(self):
        if self.sock is not None:
            self.sock.close()
        self.sock = None
        self.buffer.clear()

    def _receive(self, deadline):
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("应答超时，执行状态未知；已断开，不自动重连或重发")
            if len(self.buffer) >= HEADER.size:
                length, version, flag, service, command, sequence, reserved = HEADER.unpack(
                    self.buffer[:HEADER.size])
                if not HEADER.size <= length <= 4096 or (version, flag, service, reserved) != (1, 8, 0, 0):
                    raise ValueError("MCU响应头不合法，停止本次连接")
                if len(self.buffer) >= length:
                    body = json.loads(self.buffer[HEADER.size:length].decode("utf-8"))
                    del self.buffer[:length]
                    if not isinstance(body, dict):
                        raise ValueError("MCU响应必须为JSON对象")
                    if self.last_rx is not None and sequence != (self.last_rx + 1) & 0xFFFFFFFF:
                        raise ValueError("MCU发送流水号不连续，停止本次连接")
                    self.last_rx = sequence
                    self.event({"direction": "RX", "command": command, "seq": sequence, "body": body})
                    return command, body
            self.sock.settimeout(min(0.2, left))
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                raise ConnectionError("MCU已断开连接，执行状态未知")
            self.buffer.extend(chunk)

    def call(self, command, body=None, timeout=5.0):
        if command not in ALLOWED_COMMANDS:
            raise ValueError("验板工具只允许既定查询和050x操作，不发送算法或升级")
        if self.sock is None:
            raise ConnectionError("请先连接MCU")
        body = {} if body is None else body
        data = json.dumps(body, separators=(",", ":"), ensure_ascii=False,
                          allow_nan=False).encode("utf-8")
        if HEADER.size + len(data) > 4096:
            raise ValueError("请求超过4096字节")
        expect = {2: 1, 0x0101: 0x0102, 0x0503: 0x0504,
                  0x0507: 0x0508, 0x0601: 0x0602}.get(command, 6)
        deadline = time.monotonic() + timeout
        try:
            self.sock.sendall(HEADER.pack(HEADER.size + len(data), 1, 8, 0, command, self.tx, 0) + data)
            self.event({"direction": "TX", "command": command, "seq": self.tx, "body": body})
            self.tx = (self.tx + 1) & 0xFFFFFFFF
            while True:
                response_command, result = self._receive(deadline)
                if response_command == 6:
                    if result.get("requestCommandId") != command:
                        continue
                    status = result.get("status")
                    if status == "failed":
                        raise RemoteError(f"0x{command:04X}失败：{result.get('errorCode', '未提供原因')}")
                    if status == "accepted":
                        continue
                    if status == "completed" and expect == 6:
                        return result
                    raise ValueError("通用应答状态或方向不合法")
                if response_command == expect:
                    return result
                # 异步结果只记入日志，不当成本次设置的成功应答。
        except RemoteError:
            raise
        except Exception:
            # 超时后不能沿用连接接收同命令的迟到应答，避免错认下一次操作。
            self.close()
            raise


class Journal:
    """每轮独立目录，原始收发JSONL和逐行刷新的测量CSV。"""
    FIELDS = ("时间", "类型", "轮次", "电压点", "采样序号", "电源轨", "内存类型",
              "Boot配置", "目标mV", "AD实测mV", "AD减目标mV", "AD对目标偏差%",
              "PAC实测mA", "仪表参考mV", "AD减参考mV", "仪表参考mA", "PAC减参考mA",
              "快照年龄ms", "电源状态", "上下电结果", "RK实际启动介质", "说明")

    def __init__(self, root, configuration):
        self.path = Path(root) / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        self.path.mkdir(parents=True, exist_ok=False)
        self.events = (self.path / "frames.jsonl").open("w", encoding="utf-8")
        self.csv_file = (self.path / "measurements.csv").open("w", encoding="utf-8-sig", newline="")
        self.writer = csv.DictWriter(self.csv_file, fieldnames=self.FIELDS)
        self.writer.writeheader()
        self.csv_file.flush()
        self.event({"configuration": configuration})

    def event(self, event):
        self.events.write(json.dumps({"time": datetime.now().isoformat(timespec="milliseconds"),
                                      **event}, ensure_ascii=False, allow_nan=False) + "\n")
        self.events.flush()

    def row(self, row):
        item = {"时间": datetime.now().isoformat(timespec="milliseconds"), **row}
        self.writer.writerow(item)
        self.csv_file.flush()
        return item

    def close(self):
        self.events.close()
        self.csv_file.close()


def sample_is_fresh(sample, query_started, barrier, max_age_ms):
    """用查询发出时刻减快照年龄作保守下界，不要求PC/MCU时钟同步。"""
    age, mask = sample.get("sampleAgeMs"), sample.get("validMask")
    if (sample.get("samplePowerState") != "on" or type(mask) is not int or mask & 3 != 3
            or type(age) is not int or not 0 <= age <= max_age_ms):
        return False
    for name in ("voltageMv", "currentMa"):
        values = sample.get(name)
        if not isinstance(values, list) or len(values) != 3 or not all(finite_number(v) for v in values):
            return False
    # 留5ms边界余量；必须比调压稳定点/上次应答更新，不能反复累计同一旧快照。
    return query_started - age / 1000.0 >= barrier + 0.005


def monitor_view(sample, max_age_ms=1500, elapsed_ms=0):
    """分别检查AD/PAC有效位；无效、过期或非ON的数据留空，不以零替代。"""
    raw_age, mask = sample.get("sampleAgeMs"), sample.get("validMask")
    age = raw_age + elapsed_ms if type(raw_age) is int and raw_age >= 0 else None
    state = sample.get("samplePowerState", "未知")
    view = {"ageMs": age, "powerState": state}
    for field, bit, label in (("voltageMv", 1, "电压"), ("currentMa", 2, "电流")):
        values = sample.get(field)
        if state != "on":
            reason = "未上电/电源状态非ON"
        elif age is None:
            reason = "快照年龄未知"
        elif age > max_age_ms:
            reason = "数据过期"
        elif type(mask) is not int or not mask & bit:
            reason = "采样无效"
        elif not isinstance(values, list) or len(values) != 3 or not all(finite_number(v) for v in values):
            reason = "采样格式错误"
        else:
            reason = "有效"
        view[field] = list(values) if reason == "有效" else [None] * 3
        view[field + "Status"] = f"{label}{reason}"
    return view


class Runner:
    """循环由PC执行；每条操作等completed，异常终止，不自动重试或改变现场。"""

    def __init__(self, client, stop, event=lambda _: None, row=lambda _: None,
                 clock=time.monotonic, wait=None, sample=lambda _: None):
        self.client, self.stop, self.event, self.row = client, stop, event, row
        self.clock = clock
        self.wait = wait or stop.wait
        self.sample = sample

    def check_stop(self):
        if self.stop.is_set():
            raise Stopped()

    def pause(self, seconds):
        if self.wait(seconds):
            raise Stopped()

    def command(self, command, body=None):
        self.check_stop()
        return self.client.call(command, body)

    def state(self, expected):
        result = self.command(0x0503)
        if result.get("powerState") != expected:
            raise RuntimeError(f"电源状态不是{expected}：{result}")

    def power_off(self):
        self.command(0x0502)
        self.state("off")

    def power_on(self):
        """只按MCU当前配置上电，不暗中下电、选择类型或修改Boot。"""
        self.command(0x0501)
        self.state("on")

    def set_boot(self, mode):
        if mode not in BOOT_MV:
            raise ValueError("Boot模式不支持")
        self.command(0x0506, {"bootMode": mode})
        result = self.command(0x0507)
        if result.get("bootMode") != mode or result.get("targetMv") != BOOT_MV[mode]:
            raise RuntimeError(f"Boot配置缓存与请求不一致：{result}")

    def set_voltage(self, point, memory_type):
        """OFF时保存下次上电目标，ON时同类型立即调压；失败不自动换用标称值。"""
        self.command(0x0509, point.payload(memory_type))

    def read_monitor(self, index=1, max_age_ms=1500, kind="持续监测"):
        started = self.clock()
        raw = self.command(0x0601)
        # 加上请求耗时作为保守年龄，慢响应不能伪装为新数据。
        view = monitor_view(raw, max_age_ms, int((self.clock() - started) * 1000))
        self.sample(view)
        for i, rail in enumerate(RAILS):
            self.row({"类型": kind, "采样序号": index, "电源轨": rail,
                      "AD实测mV": view["voltageMv"][i], "PAC实测mA": view["currentMa"][i],
                      "快照年龄ms": view["ageMs"], "电源状态": view["powerState"],
                      "说明": view["voltageMvStatus"] + "；" + view["currentMaStatus"]})
        return view

    def monitor_loop(self, interval_ms, max_age_ms=1500):
        """仅查询0601，正常停止和异常均不追加电源操作；每行是查询快照。"""
        if type(interval_ms) is not int or not 100 <= interval_ms <= 60000:
            raise ValueError("监测间隔须为100～60000ms")
        index = 0
        while True:
            self.check_stop()
            index += 1
            self.read_monitor(index, max_age_ms)
            self.pause(interval_ms / 1000.0)

    def prepare(self, memory_type, boot_mode, point):
        # 每次在OFF保存该档训练目标，上电后不再追加运行期调压。
        self.power_off()
        self.set_voltage(point, memory_type)
        self.set_boot(boot_mode)
        self.command(0x0501)
        self.state("on")

    def read_fresh(self, barrier, interval_ms, max_age_ms, timeout_ms):
        deadline = self.clock() + timeout_ms / 1000.0
        while self.clock() < deadline:
            self.check_stop()
            started = self.clock()
            sample = self.command(0x0601)
            if sample_is_fresh(sample, started, barrier, max_age_ms):
                return sample
            # 无效快照最多每500ms重新查询；长记录间隔不缩减有效采样的等待机会。
            self.pause(min(interval_ms / 1000.0, 0.5, max(0.0, deadline - self.clock())))
        raise TimeoutError("未获得调压稳定后的有效新采样；停止，不用旧值或零值代替")

    def voltage_loop(self, points, cycles, memory_type, boot_mode, settle_ms,
                     samples, interval_ms, max_age_ms=1500, sample_timeout_ms=10000):
        for cycle in range(1, cycles + 1):
            for index, point in enumerate(points, 1):
                self.event(f"第{cycle}/{cycles}轮，第{index}/{len(points)}档：{point.mv} mV")
                self.prepare(memory_type, boot_mode, point)
                barrier = self.clock() + settle_ms / 1000.0
                self.pause(settle_ms / 1000.0)
                for sample_index in range(1, samples + 1):
                    sample = self.read_fresh(barrier, interval_ms, max_age_ms, sample_timeout_ms)
                    barrier = self.clock()
                    for i, rail in enumerate(RAILS):
                        mv, ma = sample["voltageMv"][i], sample["currentMa"][i]
                        ref_v, ref_i = point.reference_mv[i], point.reference_ma[i]
                        self.row({"类型": "目标电压上电", "轮次": cycle, "电压点": index, "采样序号": sample_index,
                                  "电源轨": rail, "内存类型": memory_type, "Boot配置": boot_mode,
                                  "目标mV": point.mv[i], "AD实测mV": mv, "AD减目标mV": mv - point.mv[i],
                                  "AD对目标偏差%": round((mv - point.mv[i]) / point.mv[i] * 100, 6),
                                  "PAC实测mA": ma, "仪表参考mV": ref_v,
                                  "AD减参考mV": None if ref_v is None else mv - ref_v,
                                  "仪表参考mA": ref_i, "PAC减参考mA": None if ref_i is None else ma - ref_i,
                                  "快照年龄ms": sample["sampleAgeMs"], "电源状态": "on",
                                  "说明": "OFF保存目标后上电；仅记录采样偏差，不判定DDR训练或系统启动"})
                    self.pause(interval_ms / 1000.0)

    def boot_loop(self, mode, cycles, memory_type, point, off_ms, on_ms):
        for cycle in range(1, cycles + 1):
            self.event(f"第{cycle}/{cycles}次上下电：{mode}，三轨{point.mv} mV，RK启动需外部确认")
            self.power_off()
            self.set_voltage(point, memory_type)
            self.pause(off_ms / 1000.0)
            self.set_boot(mode)
            self.command(0x0501)
            self.state("on")
            self.pause(on_ms / 1000.0)
            result = self.command(0x0507)
            if result.get("bootMode") != mode or result.get("targetMv") != BOOT_MV[mode]:
                raise RuntimeError("上电后Boot缓存发生变化")
            self.state("on")
            self.row({"类型": "Boot循环", "轮次": cycle, "内存类型": memory_type,
                      "Boot配置": mode, "目标mV": BOOT_MV[mode], "电源状态": "on",
                      "上下电结果": "指令完成", "RK实际启动介质": "未验证",
                      "说明": f"DDR目标VDD1/VDD2/VDDQ={point.mv}mV；目标mV列为Boot DAC目标，均非实测"})


def execute_run(runner, operation, power_off_at_end):
    """正常结束/用户停止才按选项下电；失败或超时保留现场，不追加控制命令。"""
    outcome = "完成"
    try:
        operation()
    except Stopped:
        outcome = "已停止"
    if power_off_at_end:
        # 用户停止不打断本条已受理操作；收完应答后执行用户勾选的收尾下电。
        runner.client.call(0x0502)
        if runner.client.call(0x0503).get("powerState") != "off":
            raise RuntimeError("收尾下电未确认，需检查现场")
        outcome += "，已确认下电"
    else:
        outcome += "，保留当前电源状态"
    return outcome
