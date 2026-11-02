"""只用127.0.0.1模拟MCU并操作验板窗口；不访问任何板卡。"""
import json
from pathlib import Path
import socket
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "b1_debug"))
from app import App
from core import BOOT_MV, HEADER


def main():
    output = Path(__file__).resolve().parents[1] / "build" / "b1-debug-qa"
    output.mkdir(parents=True, exist_ok=True)
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(10)
    records, errors, sample_overrides = [], [], []

    def server():
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(10)
                buffer = bytearray()
                sequence = 201
                mode, power, targets = "usbMaskrom", "off", [1800, 1100, 600]

                def frame(command, body):
                    nonlocal sequence
                    data = json.dumps(body, separators=(",", ":")).encode()
                    value = HEADER.pack(18 + len(data), 1, 8, 0, command, sequence, 0) + data
                    sequence += 1
                    return value

                while True:
                    chunk = connection.recv(4096)
                    if not chunk:
                        break
                    buffer.extend(chunk)
                    while len(buffer) >= 18:
                        length, _, _, _, command, _, _ = HEADER.unpack(buffer[:18])
                        if len(buffer) < length:
                            break
                        body = json.loads(buffer[18:length])
                        del buffer[:length]
                        records.append((command, body))
                        response_command = {2: 1, 0x0101: 0x0102, 0x0503: 0x0504,
                                            0x0507: 0x0508, 0x0601: 0x0602}.get(command, 6)
                        answer = {"requestCommandId": command, "status": "completed"}
                        if command == 2:
                            answer = {}
                        elif command == 0x0101:
                            answer = {"testerName": "本机模拟，非硬件", "testerVersion": "D00", "testBoard": "LOCAL"}
                        elif command == 0x0501:
                            power = "on"
                        elif command == 0x0502:
                            power = "off"
                        elif command == 0x0503:
                            answer = {"powerState": power}
                        elif command == 0x0506:
                            mode = body["bootMode"]
                        elif command == 0x0507:
                            answer = {"bootMode": mode, "targetMv": BOOT_MV[mode]}
                        elif command == 0x0509:
                            targets = [int(body[k]) for k in ("vdd1", "vdd2", "vddq")]
                        elif command == 0x0601:
                            answer = {"sampleAgeMs": 0, "samplePowerState": power, "validMask": 3,
                                      "voltageMv": [v + 1 for v in targets], "currentMa": [102, 53, 11],
                                      "statistics": None}
                            if sample_overrides:
                                answer.update(sample_overrides.pop(0))
                        else:
                            raise AssertionError(f"非验板命令：{command}")
                        response = b""
                        if command in (0x0501, 0x0502):
                            response += frame(6, {"requestCommandId": command, "status": "accepted"})
                        response += frame(response_command, answer)
                        connection.sendall(response[:9])
                        connection.sendall(response[9:])
        except Exception as exc:
            errors.append(exc)

    worker = threading.Thread(target=server, daemon=True)
    worker.start()
    root = tk.Tk()
    app = App(root)
    app.report_root = output / "reports"
    app.host.set("127.0.0.1")
    app.port.set(str(listener.getsockname()[1]))

    def wait_job():
        until = time.monotonic() + 20
        while app.busy and time.monotonic() < until:
            root.update()
            time.sleep(0.01)
        root.update()
        assert not app.busy, "窗口工作未完成"
        assert "停止：" not in app.status.get(), app.status.get()

    def check_layout():
        # 检查两个窗口尺寸下，当前页面的可见控件都在客户区；实际外观另外人工截图检查。
        def check_children(parent):
            for widget in parent.winfo_children():
                if widget.winfo_ismapped():
                    x, y = widget.winfo_rootx() - root.winfo_rootx(), widget.winfo_rooty() - root.winfo_rooty()
                    assert x >= 0 and y >= 0, f"控件越界：{widget}"
                    assert x + widget.winfo_width() <= root.winfo_width(), f"控件右侧被截断：{widget}"
                    assert y + widget.winfo_height() <= root.winfo_height(), f"控件底部被截断：{widget}"
                    check_children(widget)
        for size in ("1050x760", "1180x860"):
            root.geometry(size)
            root.update()
            check_children(root)

    try:
        app.connect()
        wait_job()
        assert "本机模拟" in app.status.get()
        check_layout()
        app.manual("应用三轨电压")
        wait_job()
        app.boot_mode.set("emmc")
        app.manual("设置Boot")
        wait_job()
        app.manual("上电")
        wait_job()
        assert [c for c, _ in records[3:]] == [0x0509, 0x0506, 0x0507, 0x0501, 0x0503]
        for var, value in zip(app.targets, (1810, 1105, 605)):
            var.set(str(value))
        app.manual("应用三轨电压")
        wait_job()
        assert records[-1] == (0x0509, {"memoryType": "LPDDR4X", "vdd1": "1810", "vdd2": "1105", "vddq": "605"})
        start = len(records)
        app.monitor_ms.set("100")
        sample_overrides.extend([{"validMask": 1}, {"sampleAgeMs": 5000}])
        app.start_monitor()
        until = time.monotonic() + 5
        while len(app.live_history) < 4 and time.monotonic() < until:
            root.update()
            time.sleep(0.01)
        assert app.busy and len(app.live_history) >= 4
        assert not app.settings_visible
        assert all(c.winfo_height() > 100 for c in app.canvases)
        assert any(row[1]["currentMa"] == [None] * 3 for row in app.live_history)
        assert any(row[1]["voltageMv"] == [None] * 3 for row in app.live_history)
        check_layout()
        app.stop()
        wait_job()
        assert all(c == 0x0601 for c, _ in records[start:]), "监测不得追加任何控制命令"
        app.last_sample = (time.monotonic() - 3, app.last_sample[1])
        app.refresh_live()
        assert "停止更新" in app.live_summary.get()
        assert all("— mV" in var.get() for var in app.live_values)
        app.query(0x0601)
        wait_job()
        assert "1811 mV" in app.live_values[0].get()
        app.manual("下电")
        wait_job()
        assert [c for c, _ in records[-2:]] == [0x0502, 0x0503]
        app.toggle_settings()
        app.book.select(1)
        app.clear_points()
        for var, value in zip(app.targets, (3350, 1675, 1675)):
            var.set(str(value))
        app.add_point()
        app.samples.set("2")
        app.settle_ms.set("100")
        app.interval_ms.set("100")
        app.start_voltage()
        assert str(app.connect_button["state"]) == "disabled"
        wait_job()
        assert len(app.table.get_children()) == 6
        assert "已确认下电" in app.status.get()
        check_layout()
        app.book.select(2)
        app.boot_mode.set("fspiM1")
        app.boot_cycles.set("2")
        app.off_ms.set("0")
        app.on_ms.set("500")
        app.start_boot()
        wait_job()
        assert len(app.table.get_children()) == 2
        assert all(app.table.item(k, "values")[-1] == "未验证" for k in app.table.get_children())
        check_layout()
        assert not any(c in (0x0505, 0x0103, 0x0901) for c, _ in records)
        (output / "gui_result.json").write_text(json.dumps(
            {"passed": True, "endpoint": "127.0.0.1", "requestCount": len(records),
             "voltageRows": 6, "bootRows": 2, "hardwareAccess": False}, indent=2), encoding="utf-8")
        print(f"GUI本机回环及控件边界检查通过，共{len(records)}条请求；记录：{output}")
        if "--preview" in sys.argv:
            # 仅保留本机模拟窗口供截图检查，超时退出不访问开发板。
            root.title("B1 界面验收 · 仅本机模拟")
            app.manual("上电")
            wait_job()
            app.book.select(0)
            app.start_monitor()
            root.after(45000, root.quit)
            root.mainloop()
            app.stop()
            wait_job()
    finally:
        app.client.close()
        root.destroy()
        worker.join(3)
        listener.close()
    if errors:
        raise errors[0]


if __name__ == "__main__":
    main()
