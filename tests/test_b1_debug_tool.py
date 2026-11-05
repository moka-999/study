"""验板工具的本机协议、采样新鲜度和循环失败路径回归；不访问开发板。"""
import csv
import json
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time
import unittest
from contextlib import contextmanager

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "b1_debug"))
from core import (BOOT_MV, HEADER, Client, Journal, RemoteError, Runner, Stopped,
                  VoltagePoint, execute_run, monitor_view, sample_is_fresh)


def packet(command, sequence, body):
    data = json.dumps(body, separators=(",", ":")).encode()
    return HEADER.pack(18 + len(data), 1, 8, 0, command, sequence, 0) + data


def receive_request(conn):
    def exact(count):
        data = bytearray()
        while len(data) < count:
            chunk = conn.recv(count - len(data))
            if not chunk:
                raise EOFError("本机客户端提前断开")
            data.extend(chunk)
        return data
    header = HEADER.unpack(exact(18))
    return header[4], header[5], json.loads(exact(header[0] - 18))


@contextmanager
def local_server(handler):
    """只绑定127.0.0.1，由测试代码明确提供对端行为。"""
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(3)
    errors = []

    def work():
        try:
            conn, _ = listener.accept()
            with conn:
                conn.settimeout(3)
                handler(conn)
        except Exception as exc:
            errors.append(exc)

    worker = threading.Thread(target=work, daemon=True)
    worker.start()
    try:
        yield listener.getsockname()[1]
    finally:
        worker.join(4)
        listener.close()
        if worker.is_alive():
            raise AssertionError("本机服务线程未退出")
        if errors:
            raise errors[0]


class FakeClock:
    def __init__(self):
        self.now = 100.0

    def __call__(self):
        return self.now

    def wait(self, seconds):
        self.now += seconds
        return False


def good_sample(age=10):
    return {"sampleAgeMs": age, "samplePowerState": "on", "validMask": 3,
            "voltageMv": [1801, 1102, 599], "currentMa": [100, 50, 10], "statistics": None}


class BoardModel:
    """提供050x最小合同，分别记录OFF缓存与实际发起上电的目标。"""
    def __init__(self, clock):
        self.clock = clock
        self.calls = []
        self.power = "off"
        self.mode = "usbMaskrom"
        self.failed_command = None
        self.samples = None
        self.training_mv = (1800, 1100, 600)
        self.start_targets = []
        self.voltage_states = []

    def call(self, command, body=None):
        self.calls.append((command, body))
        self.clock.now += 0.01
        if command == self.failed_command:
            raise RemoteError("hardwareError")
        if command == 0x0501:
            self.power = "on"
            self.start_targets.append(self.training_mv)
        elif command == 0x0502:
            self.power = "off"
        elif command == 0x0503:
            return {"powerState": self.power}
        elif command == 0x0506:
            if self.power != "off":
                raise RemoteError("powerMustBeOff")
            self.mode = body["bootMode"]
        elif command == 0x0507:
            return {"bootMode": self.mode, "targetMv": BOOT_MV[self.mode]}
        elif command == 0x0509:
            self.voltage_states.append(self.power)
            if self.power == "off":
                self.training_mv = tuple(int(body[k]) for k in ("vdd1", "vdd2", "vddq"))
        elif command == 0x0601:
            return next(self.samples) if self.samples is not None else good_sample(0)
        return {"status": "completed", "requestCommandId": command}


class ValidationTests(unittest.TestCase):
    def test_full_tps_range_and_exact_steps(self):
        for mv in ((800, 400, 400), (3350, 1675, 1675), (2000, 1205, 655)):
            for kind in ("LPDDR4", "LPDDR4X"):
                data = VoltagePoint(mv).payload(kind)
                self.assertEqual(list(data), ["memoryType", "vdd1", "vdd2", "vddq"])
                self.assertEqual(data["vdd1"], str(mv[0]))
        for mv in ((790, 1100, 600), (3360, 1100, 600), (1805, 1100, 600),
                   (1800, 1101, 600), (1800, 1100, 1680), (True, 1100, 600)):
            with self.subTest(mv=mv), self.assertRaises(ValueError):
                VoltagePoint(mv)

    def test_reference_is_optional_but_finite(self):
        VoltagePoint((1800, 1100, 600))
        for value in (float("nan"), float("inf"), -1, "1", True):
            with self.subTest(value=value), self.assertRaises(ValueError):
                VoltagePoint((1800, 1100, 600), (value, None, None))

    def test_freshness_checks_oldest_acquisition_after_barrier(self):
        self.assertTrue(sample_is_fresh(good_sample(100), 11, 10, 1500))
        self.assertFalse(sample_is_fresh(good_sample(100), 10.05, 10, 1500))
        self.assertFalse(sample_is_fresh(good_sample(0), 10, 10, 1500))
        self.assertFalse(sample_is_fresh(good_sample(1501), 20, 10, 1500))

    def test_invalid_samples_never_become_zero_or_pass(self):
        changes = ({"samplePowerState": "off"}, {"validMask": 1}, {"validMask": True},
                   {"sampleAgeMs": -1}, {"sampleAgeMs": "0"}, {"voltageMv": []},
                   {"voltageMv": [1800, float("nan"), 600]}, {"currentMa": None})
        for change in changes:
            with self.subTest(change=change):
                self.assertFalse(sample_is_fresh({**good_sample(), **change}, 20, 10, 1500))

    def test_monitor_groups_are_independent_and_invalid_is_not_zero(self):
        view = monitor_view({**good_sample(), "validMask": 1, "currentMa": [999] * 3})
        self.assertEqual(view["voltageMv"], [1801, 1102, 599])
        self.assertEqual(view["currentMa"], [None] * 3)
        self.assertIn("无效", view["currentMaStatus"])
        view = monitor_view({**good_sample(), "voltageMv": [1800, None, 600]})
        self.assertEqual(view["voltageMv"], [None] * 3)
        self.assertEqual(view["currentMa"], [100, 50, 10])

    def test_monitor_rejects_old_off_or_malformed_snapshots(self):
        for change in ({"sampleAgeMs": -1}, {"sampleAgeMs": "0"},
                       {"sampleAgeMs": 1501}, {"samplePowerState": "off"}, {"validMask": True}):
            with self.subTest(change=change):
                view = monitor_view({**good_sample(), **change})
                self.assertEqual(view["voltageMv"], [None] * 3)
                self.assertEqual(view["currentMa"], [None] * 3)
        self.assertEqual(monitor_view(good_sample(1400), elapsed_ms=101)["voltageMv"], [None] * 3)
        self.assertEqual(monitor_view({**good_sample(), "currentMa": [0, 0, 0]})["currentMa"], [0, 0, 0])


class ProtocolTests(unittest.TestCase):
    def test_fragmented_and_coalesced_responses_use_independent_sequences(self):
        received = []
        events = []
        def handler(conn):
            received.append(receive_request(conn))
            data = packet(6, 0xFFFFFFFE, {"requestCommandId": 0x0502, "status": "accepted"})
            data += packet(6, 0xFFFFFFFF, {"requestCommandId": 0x0502, "status": "completed"})
            conn.sendall(data[:7])
            time.sleep(0.01)
            conn.sendall(data[7:])
            received.append(receive_request(conn))
            conn.sendall(packet(0x0504, 0, {"powerState": "off"}))
        with local_server(handler) as port:
            client = Client(events.append)
            client.connect("127.0.0.1", port)
            client.tx = 0xFFFFFFFF
            try:
                self.assertEqual(client.call(0x0502)["status"], "completed")
                self.assertEqual(client.call(0x0503)["powerState"], "off")
            finally:
                client.close()
        self.assertEqual([r[1] for r in received], [0xFFFFFFFF, 0])
        self.assertEqual([e["seq"] for e in events if e["direction"] == "RX"], [0xFFFFFFFE, 0xFFFFFFFF, 0])

    def test_unrelated_async_packet_is_not_command_completion(self):
        def handler(conn):
            receive_request(conn)
            conn.sendall(packet(0x0105, 91, {"algorithmName": "mats"}) +
                         packet(6, 92, {"requestCommandId": 0x0506, "status": "completed"}) +
                         packet(6, 93, {"requestCommandId": 0x0502, "status": "completed"}))
        with local_server(handler) as port:
            client = Client()
            client.connect("127.0.0.1", port)
            try:
                self.assertEqual(client.call(0x0502)["requestCommandId"], 0x0502)
            finally:
                client.close()

    def test_busy_stops_operation_without_closing_readonly_diagnosis_path(self):
        def handler(conn):
            receive_request(conn)
            conn.sendall(packet(6, 1, {"requestCommandId": 0x0502, "status": "failed", "errorCode": "busy"}))
            self.assertEqual(receive_request(conn)[0], 0x0503)
            conn.sendall(packet(0x0504, 2, {"powerState": "on"}))
        with local_server(handler) as port:
            client = Client()
            client.connect("127.0.0.1", port)
            try:
                with self.assertRaisesRegex(RemoteError, "busy"):
                    client.call(0x0502)
                self.assertEqual(client.call(0x0503)["powerState"], "on")
            finally:
                client.close()

    def test_timeout_closes_connection_and_does_not_retry(self):
        received = []
        def handler(conn):
            received.append(receive_request(conn))
            conn.sendall(packet(6, 1, {"requestCommandId": 0x0501, "status": "accepted"}))
            self.assertEqual(conn.recv(1024), b"")
        with local_server(handler) as port:
            client = Client()
            client.connect("127.0.0.1", port)
            with self.assertRaises(TimeoutError):
                client.call(0x0501, timeout=0.08)
            self.assertIsNone(client.sock)
        self.assertEqual(len(received), 1)

    def test_invalid_header_and_sequence_close_connection(self):
        bad_packets = [HEADER.pack(4097, 1, 8, 0, 1, 1, 0),
                       packet(0x0105, 20, {}) + packet(1, 22, {})]
        for bad in bad_packets:
            def handler(conn):
                receive_request(conn)
                conn.sendall(bad)
                self.assertEqual(conn.recv(1024), b"")
            with self.subTest(bad=bad), local_server(handler) as port:
                client = Client()
                client.connect("127.0.0.1", port)
                with self.assertRaises(ValueError):
                    client.call(2)
                self.assertIsNone(client.sock)

    def test_forbids_algorithm_upgrade_and_ready_dependent_restart(self):
        for command in (0x0103, 0x0108, 0x010A, 0x0901, 0x0505):
            with self.subTest(command=command), self.assertRaises(ValueError):
                Client().call(command)

    def test_stop_during_accepted_waits_for_completed(self):
        stop = threading.Event()
        events = []
        def handler(conn):
            self.assertEqual(receive_request(conn)[0], 0x0502)
            conn.sendall(packet(6, 41, {"requestCommandId": 0x0502, "status": "accepted"}))
            stop.set()
            conn.sendall(packet(6, 42, {"requestCommandId": 0x0502, "status": "completed"}))
            self.assertEqual(conn.recv(1024), b"")
        with local_server(handler) as port:
            client = Client(events.append)
            client.connect("127.0.0.1", port)
            runner = Runner(client, stop)
            try:
                outcome = execute_run(runner, lambda: runner.prepare(
                    "LPDDR4X", "emmc", VoltagePoint((1700, 1060, 570))), False)
                self.assertIn("已停止", outcome)
            finally:
                client.close()
        self.assertEqual([e["seq"] for e in events if e["direction"] == "RX"], [41, 42])


class LoopTests(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.client = BoardModel(self.clock)
        self.stop = threading.Event()
        self.rows = []
        self.runner = Runner(self.client, self.stop, row=self.rows.append,
                             clock=self.clock, wait=self.clock.wait)

    def test_single_steps_do_not_hide_power_cycles(self):
        self.runner.set_voltage(VoltagePoint((1700, 1060, 570)), "LPDDR4X")
        self.runner.set_boot("emmc")
        self.runner.power_on()
        self.runner.set_voltage(VoltagePoint((2000, 1205, 655)), "LPDDR4X")
        self.assertEqual([c for c, _ in self.client.calls],
                         [0x0509, 0x0506, 0x0507, 0x0501, 0x0503, 0x0509])
        self.assertEqual(self.client.power, "on")
        self.assertEqual(self.client.start_targets, [(1700, 1060, 570)])
        self.assertEqual(self.client.training_mv, (1700, 1060, 570))
        self.runner.power_off()
        self.assertEqual(self.client.calls[-2:], [(0x0502, None), (0x0503, None)])

    def test_single_step_rejection_does_not_change_power(self):
        self.client.failed_command = 0x0509
        with self.assertRaises(RemoteError):
            self.runner.set_voltage(VoltagePoint((2000, 1200, 600)), "LPDDR4X")
        self.assertEqual([c for c, _ in self.client.calls], [0x0509])
        self.assertEqual(self.client.power, "off")
        self.client.calls.clear()
        self.client.power = "on"
        with self.assertRaises(RemoteError):
            self.runner.set_boot("emmc")
        self.assertEqual([c for c, _ in self.client.calls], [0x0506])
        self.assertEqual(self.client.power, "on")
        with self.assertRaises(ValueError):
            self.runner.set_boot("not-a-mode")
        self.assertEqual(len(self.client.calls), 1)

    def test_monitor_is_readonly_and_continues_after_invalid_data(self):
        self.client.power = "on"
        self.client.samples = iter([{**good_sample(), "validMask": 1}, good_sample(5000), good_sample(0)])
        views = []
        def sample(view):
            views.append(view)
            if len(views) == 3:
                self.stop.set()
        self.runner.sample = sample
        result = execute_run(self.runner, lambda: self.runner.monitor_loop(100), False)
        self.assertIn("已停止", result)
        self.assertEqual([c for c, _ in self.client.calls], [0x0601] * 3)
        self.assertEqual(self.client.power, "on")
        self.assertEqual(len(self.rows), 9)
        self.assertIsNone(self.rows[0]["PAC实测mA"])
        self.assertIsNone(self.rows[3]["AD实测mV"])
        self.assertEqual(self.rows[6]["AD实测mV"], 1801)

    def test_monitor_error_stops_without_power_cleanup_or_retry(self):
        self.client.power = "on"
        self.client.failed_command = 0x0601
        with self.assertRaises(RemoteError):
            execute_run(self.runner, lambda: self.runner.monitor_loop(500), False)
        self.assertEqual([c for c, _ in self.client.calls], [0x0601])
        self.assertEqual(self.client.power, "on")

    def test_monitor_bad_interval_and_preexisting_stop_send_nothing(self):
        for value in (0, 99, 60001, True):
            with self.assertRaises(ValueError):
                self.runner.monitor_loop(value)
        self.stop.set()
        with self.assertRaises(Stopped):
            self.runner.monitor_loop(500)
        self.assertEqual(self.client.calls, [])

    def test_voltage_loop_order_ranges_and_deviations(self):
        points = [VoltagePoint((2000, 1205, 655), (1800, None, None), (95, None, None)),
                  VoltagePoint((3350, 1675, 1675))]
        execute_run(self.runner, lambda: self.runner.voltage_loop(
            points, 2, "LPDDR4X", "fspiM1", 100, 2, 100), True)
        commands = [c for c, _ in self.client.calls]
        self.assertEqual(commands[:7], [0x0502, 0x0503, 0x0509, 0x0506, 0x0507, 0x0501, 0x0503])
        targets = [body["vdd1"] for cmd, body in self.client.calls if cmd == 0x0509]
        self.assertEqual(targets, ["2000", "3350", "2000", "3350"])
        self.assertEqual(self.client.start_targets, [p.mv for p in points] * 2)
        self.assertEqual(self.client.voltage_states, ["off"] * 4)
        self.assertEqual(commands.count(0x0501), 4)
        self.assertEqual(commands[-2:], [0x0502, 0x0503])
        self.assertEqual(len(self.rows), 24)
        self.assertEqual(self.rows[0]["AD减目标mV"], -199)
        self.assertEqual(self.rows[0]["AD减参考mV"], 1)
        self.assertEqual(self.rows[0]["PAC减参考mA"], 5)
        self.assertIsNone(self.rows[1]["AD减参考mV"])

    def test_old_snapshots_are_discarded_and_no_duplicates_counted(self):
        self.client.samples = iter([good_sample(1500), good_sample(0), good_sample(1500), good_sample(0)])
        self.runner.voltage_loop([VoltagePoint((1800, 1100, 600))], 1, "LPDDR4X", "emmc", 0, 2, 100)
        self.assertEqual(len(self.rows), 6)
        self.assertEqual(sum(c == 0x0601 for c, _ in self.client.calls), 4)

    def test_sampling_timeout_does_not_change_power_after_failure(self):
        self.client.samples = iter([{"validMask": 0}] * 100)
        with self.assertRaises(TimeoutError):
            execute_run(self.runner, lambda: self.runner.voltage_loop(
                [VoltagePoint((1800, 1100, 600))], 1, "LPDDR4X", "emmc", 0, 1, 60000,
                sample_timeout_ms=1000), True)
        self.assertEqual(self.client.power, "on")
        self.assertEqual(self.client.calls[-1][0], 0x0601)
        self.assertEqual(self.rows, [])

    def test_boot_loop_never_claims_actual_medium_or_waits_for_rk(self):
        point = VoltagePoint((1900, 1150, 1150))
        execute_run(self.runner, lambda: self.runner.boot_loop("fspiM1", 3, "LPDDR4", point, 500, 3000), True)
        commands = [c for c, _ in self.client.calls]
        self.assertNotIn(0x0505, commands)
        self.assertEqual(commands.count(0x0501), 3)
        self.assertEqual(commands.count(0x0502), 4)
        self.assertEqual(len(self.rows), 3)
        self.assertTrue(all(r["RK实际启动介质"] == "未验证" for r in self.rows))
        self.assertEqual(self.client.calls[2][1]["vddq"], "1150")
        self.assertEqual(self.client.start_targets, [point.mv] * 3)
        self.assertEqual(self.client.voltage_states, ["off"] * 3)

    def test_off_preset_rejected_stops_without_nominal_fallback_or_power_on(self):
        self.client.failed_command = 0x0509
        with self.assertRaises(RemoteError):
            execute_run(self.runner, lambda: self.runner.voltage_loop(
                [VoltagePoint((1700, 1060, 570))], 1, "LPDDR4X", "emmc", 0, 1, 100), True)
        self.assertEqual([c for c, _ in self.client.calls], [0x0502, 0x0503, 0x0509])
        self.assertEqual(self.client.power, "off")
        self.assertEqual(self.rows, [])

    def test_driver_failure_leaves_scene_and_no_auto_cleanup(self):
        self.client.failed_command = 0x0506
        with self.assertRaises(RemoteError):
            execute_run(self.runner, lambda: self.runner.boot_loop(
                "emmc", 3, "LPDDR4X", VoltagePoint((1800, 1100, 600)), 0, 1000), True)
        self.assertEqual(self.client.calls[-1][0], 0x0506)
        self.assertNotIn(0x0501, [c for c, _ in self.client.calls])

    def test_manual_stop_obeys_explicit_cleanup_selection(self):
        def operation():
            self.client.power = "on"
            raise Stopped()
        self.assertIn("已确认下电", execute_run(self.runner, operation, True))
        self.assertEqual([c for c, _ in self.client.calls], [0x0502, 0x0503])
        self.client.calls.clear()
        self.assertIn("保留", execute_run(self.runner, operation, False))
        self.assertEqual(self.client.calls, [])

    def test_journal_flushes_utf8_csv_and_raw_events(self):
        with tempfile.TemporaryDirectory() as root:
            journal = Journal(root, {"范围": "TPS完整范围"})
            journal.event({"direction": "RX", "command": 0x0602, "seq": 7, "body": good_sample()})
            journal.row({"类型": "调压", "PAC实测mA": 100, "仪表参考mA": None})
            path = journal.path
            self.assertTrue((path / "measurements.csv").read_bytes().startswith(b"\xef\xbb\xbf"))
            journal.close()
            with (path / "measurements.csv").open(encoding="utf-8-sig", newline="") as stream:
                row = next(csv.DictReader(stream))
            self.assertEqual(row["仪表参考mA"], "")
            self.assertEqual(row["PAC实测mA"], "100")
            self.assertEqual(len((path / "frames.jsonl").read_text(encoding="utf-8").splitlines()), 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
