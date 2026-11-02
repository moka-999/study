"""B1外设验板图形窗口；网络在单个工作线程执行，界面线程不访问socket。"""
from __future__ import annotations

import json
import os
import queue
import threading
import time
from collections import deque
from dataclasses import asdict
from datetime import datetime
from pathlib import Path
import tkinter as tk
from tkinter import ttk, messagebox

from core import (BOOT_MV, LIMITS, RAILS, Client, Journal, Runner,
                  VoltagePoint, execute_run, nominal)


def default_report_root():
    return Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "JMCT" / "B1Debug" / "reports"


class App:
    def __init__(self, root):
        self.root = root
        root.title("JMCT · B1 外设验板")
        root.geometry("1180x860")
        root.minsize(1050, 760)
        root.option_add("*Font", ("Microsoft YaHei UI", 10))
        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure("TButton", padding=(10, 5))
        style.configure("Treeview", rowheight=25)
        style.configure("Title.TLabel", font=("Microsoft YaHei UI", 17, "bold"))
        self.events = queue.Queue()
        self.stop_event = threading.Event()
        self.client = Client(self.wire_event)
        self.journal = None
        self.report_root = default_report_root()
        self.last_report = None
        self.busy, self.closing = False, False
        self.points = {}
        self.targets = [tk.StringVar(value=str(v)) for v in nominal("LPDDR4X")]
        self.live_history = deque(maxlen=300)
        self.last_sample = None
        self.live_expired = False
        self.settings_visible = True
        self.controls = []
        self.actions = []
        self.host = tk.StringVar(value="192.168.1.201")
        self.port = tk.StringVar(value="8888")
        self.memory_type = tk.StringVar(value="LPDDR4X")
        self.boot_mode = tk.StringVar(value="usbMaskrom")
        self.power_off_at_end = tk.BooleanVar(value=True)
        self.status = tk.StringVar(value="未连接。请先关闭其他PC/网络助手连接。")
        self.report_text = tk.StringVar(value=f"记录目录：{self.report_root}")
        self.endpoint = ""
        self.connected_text = tk.StringVar(value="未连接")
        self.build()
        self.update_controls()
        root.protocol("WM_DELETE_WINDOW", self.close_window)
        root.after(80, self.drain)

    def control(self, widget, state="normal"):
        self.controls.append((widget, state))
        return widget

    def entry(self, parent, label, value, column, low=None, high=None, step=1, width=13):
        ttk.Label(parent, text=label).grid(row=0, column=column, sticky="w", padx=5)
        var = value if isinstance(value, tk.Variable) else tk.StringVar(value=str(value))
        kwargs = dict(textvariable=var, width=width)
        widget = ttk.Entry(parent, **kwargs) if low is None else ttk.Spinbox(
            parent, from_=low, to=high, increment=step, **kwargs)
        self.control(widget).grid(row=1, column=column, sticky="ew", padx=5, pady=(3, 6))
        return var

    def button(self, parent, label, command, connected=True):
        widget = ttk.Button(parent, text=label, command=command)
        self.actions.append((widget, connected))
        return widget

    def build(self):
        shell = ttk.Frame(self.root, padding=14)
        shell.pack(fill="both", expand=True)
        ttk.Label(shell, text="B1 外设验板", style="Title.TLabel").pack(anchor="w")
        ttk.Label(shell, text="单步控制与持续监测  /  电压及Boot循环  /  CSV自动记录").pack(anchor="w", pady=(2, 8))
        connection = ttk.Frame(shell)
        connection.pack(fill="x")
        self.entry(connection, "MCU IP", self.host, 0)
        self.entry(connection, "端口", self.port, 1)
        self.connect_button = self.button(connection, "连接 / 查询设备", self.connect, False)
        self.connect_button.grid(row=1, column=2, padx=8)
        self.button(connection, "断开", lambda: self.job("断开", lambda: self.client.close())).grid(row=1, column=3)
        self.button(connection, "查询电源", lambda: self.query(0x0503)).grid(row=1, column=4, padx=8)
        self.button(connection, "读取电压 / 电流", lambda: self.query(0x0601)).grid(row=1, column=5)
        self.button(connection, "下电", lambda: self.manual("下电")).grid(row=1, column=6, padx=8)
        ttk.Label(connection, textvariable=self.connected_text).grid(row=0, column=2, columnspan=5, sticky="w", padx=8)

        config = ttk.Frame(shell)
        config.pack(fill="x", pady=(3, 8))
        ttk.Label(config, text="内存类型").pack(side="left", padx=(5, 5))
        self.control(ttk.Combobox(config, textvariable=self.memory_type, values=("LPDDR4X", "LPDDR4"),
                                  width=12, state="readonly"), "readonly").pack(side="left")
        ttk.Label(config, text="Boot模式").pack(side="left", padx=(24, 5))
        self.control(ttk.Combobox(config, textvariable=self.boot_mode, values=tuple(BOOT_MV),
                                  width=15, state="readonly"), "readonly").pack(side="left")
        ttk.Label(config, text="两种循环都会先下电，再按标称电压上电；不需要RK心跳。").pack(side="left", padx=15)
        book = ttk.Notebook(shell)
        self.book = book
        book.pack(fill="x")
        manual = ttk.Frame(book, padding=12)
        volt = ttk.Frame(book, padding=10)
        boot = ttk.Frame(book, padding=14)
        book.add(manual, text="  单步 / 持续监测  ")
        book.add(volt, text="  三轨电压循环  ")
        book.add(boot, text="  Boot / 上下电循环  ")
        ttk.Label(manual, text="只执行点选动作，不自动下电或重启。上方内存类型、Boot选择须点击对应设置按钮才生效。",
                  wraplength=1000).pack(anchor="w", pady=4)
        manual_voltage = ttk.Frame(manual)
        manual_voltage.pack(fill="x", pady=8)
        for i, (rail, limits) in enumerate(zip(RAILS, LIMITS)):
            low, high, step = limits
            self.entry(manual_voltage, f"{rail} · {low}～{high}mV / {step}mV",
                       self.targets[i], i, low, high, step, width=18)
        manual_buttons = ttk.Frame(manual)
        manual_buttons.pack(fill="x", pady=8)
        for label, name in (("上电（使用MCU当前配置）", "上电"), ("设置Boot", "设置Boot"),
                            ("应用三轨电压 / 类型", "应用三轨电压")):
            self.button(manual_buttons, label, lambda action=name: self.manual(action)).pack(side="left", padx=(0, 8))
        self.control(ttk.Button(manual_buttons, text="填入标称值", command=self.fill_nominal)).pack(side="left")
        ttk.Label(manual, text="首次操作：OFF时应用标称值选择内存类型 → 设置Boot → 上电 → 按需调压。\n"
                  "任意调压须处于ON；设置Boot须处于OFF。完整TPS范围开放，负载承受范围由操作者确认。",
                  wraplength=1000, foreground="#954600").pack(anchor="w", pady=8)
        monitor_bar = ttk.Frame(manual)
        monitor_bar.pack(fill="x", pady=8)
        self.monitor_ms = self.entry(monitor_bar, "持续查询间隔ms", 500, 0, 100, 60000)
        self.button(monitor_bar, "开始持续监测（只读）", self.start_monitor).grid(row=1, column=1, padx=12)
        ttk.Label(manual, text="监测只查询0601；停止/关闭监测不会下电。先停止监测，再做单步设置；数据超过1500ms显示过期。",
                  wraplength=1000).pack(anchor="w", pady=4)
        ttk.Label(volt, text="开放TPS可编码范围，器件可写不等于板上负载安全。只执行下面清单，不自动扫描全范围。",
                  foreground="#954600").pack(anchor="w")
        voltage_boxes = ttk.Frame(volt)
        voltage_boxes.pack(fill="x", pady=5)
        self.references_v, self.references_i = [], []
        for i, (rail, limits) in enumerate(zip(RAILS, LIMITS)):
            low, high, step = limits
            group = ttk.LabelFrame(voltage_boxes, text=f"{rail} · {low}～{high}mV / {step}mV", padding=5)
            group.grid(row=0, column=i, sticky="ew", padx=3)
            voltage_boxes.columnconfigure(i, weight=1)
            self.entry(group, "目标mV", self.targets[i], 0, low, high, step, width=8)
            self.references_v.append(self.entry(group, "参考mV（可空）", "", 1, width=10))
            self.references_i.append(self.entry(group, "参考mA（可空）", "", 2, width=10))
        ttk.Label(volt, text="参考值由你填入仪表读数；留空时仅计算AD与目标电压的偏差，PAC电流不作判定。").pack(anchor="w")
        planbar = ttk.Frame(volt)
        planbar.pack(fill="x", pady=5)
        for label, fn in (("添加这一档", self.add_point), ("删除选中", self.remove_point),
                          ("清空清单", self.clear_points), ("标称值填入输入框", self.fill_nominal)):
            self.control(ttk.Button(planbar, text=label, command=fn)).pack(side="left", padx=(0, 8))
        columns = ("v1", "v2", "vq", "refv", "refi")
        self.plan = ttk.Treeview(volt, columns=columns, show="headings", height=3, selectmode="extended")
        for key, title, width in zip(columns, ("VDD1 mV", "VDD2 mV", "VDDQ mV", "三路参考mV", "三路参考mA"), (120, 120, 120, 290, 290)):
            self.plan.heading(key, text=title)
            self.plan.column(key, width=width, anchor="center")
        self.plan.pack(fill="x")
        self.add_point()
        settings = ttk.Frame(volt)
        settings.pack(fill="x", pady=(5, 0))
        self.volt_cycles = self.entry(settings, "清单循环次数", 1, 0, 1, 100000)
        self.settle_ms = self.entry(settings, "每档稳定等待ms", 1000, 1, 0, 600000)
        self.samples = self.entry(settings, "每档新采样数", 5, 2, 1, 10000)
        self.interval_ms = self.entry(settings, "查询间隔ms", 500, 3, 100, 60000)
        self.button(settings, "开始调压循环（先下电）", self.start_voltage).grid(row=1, column=4, padx=12)

        ttk.Label(boot, text="每次执行：下电 → 等待 → 写Boot DAC并查询配置 → 标称电压上电 → 观察等待。",
                  wraplength=1000).pack(anchor="w", pady=8)
        ttk.Label(boot, text="0508返回的是配置缓存，不能证明RK实际启动介质。未烧SPL时不使用0505，也不发送测试算法。",
                  wraplength=1000).pack(anchor="w", pady=8)
        ttk.Label(boot, text="报告将实际启动介质记为“未验证”；应结合Boot电压测量、RK侧证据或启动总线观测验收。",
                  foreground="#954600", wraplength=1000).pack(anchor="w", pady=8)
        boot_settings = ttk.Frame(boot)
        boot_settings.pack(fill="x", pady=12)
        self.boot_cycles = self.entry(boot_settings, "循环次数", 10, 0, 1, 100000)
        self.off_ms = self.entry(boot_settings, "额外下电等待ms", 500, 1, 0, 600000)
        self.on_ms = self.entry(boot_settings, "上电观察时间ms", 3000, 2, 500, 600000)
        self.button(boot_settings, "开始Boot循环（先下电）", self.start_boot).grid(row=1, column=3, padx=12)
        ttk.Label(boot, text="无有效镜像时，RK可能回退到USB；仅看到USB设备也不能证明原先选中了目标介质。",
                  wraplength=1000).pack(anchor="w", pady=12)

        footer = ttk.Frame(shell)
        self.footer = footer
        footer.pack(fill="x", pady=8)
        self.control(ttk.Checkbutton(footer, text="仅循环：正常结束 / 手动停止后下电", variable=self.power_off_at_end)).pack(side="left")
        self.stop_button = ttk.Button(footer, text="停止", command=self.stop)
        self.stop_button.pack(side="left", padx=12)
        ttk.Label(footer, text="异常立即停止，不自动重试或追加下电；断开TCP不会切断电源。").pack(side="left")
        self.settings_button = ttk.Button(footer, text="收起设置", command=self.toggle_settings)
        self.settings_button.pack(side="right")
        ttk.Label(shell, textvariable=self.status, foreground="#135e78", wraplength=1090).pack(anchor="w", pady=(0, 5))
        reportbar = ttk.Frame(shell)
        reportbar.pack(side="bottom", fill="x", pady=(5, 0))
        ttk.Label(reportbar, textvariable=self.report_text, wraplength=850).pack(side="left")
        ttk.Button(reportbar, text="打开记录目录", command=self.open_report).pack(side="right")
        output = ttk.Notebook(shell)
        self.output = output
        output.pack(fill="both", expand=True)
        live = ttk.Frame(output)
        self.live_panel = live
        results = ttk.Frame(output)
        self.results_panel = results
        logs = ttk.Frame(output)
        output.add(live, text="  实时读数 / 曲线  ")
        output.add(results, text="  测量记录  ")
        output.add(logs, text="  通信日志  ")
        self.live_summary = tk.StringVar(value="尚未查询；电压来自AD，电流来自PAC。曲线是PC查询快照，不代表ADC采样速率。")
        ttk.Label(live, textvariable=self.live_summary, wraplength=1000).pack(anchor="w", padx=6, pady=3)
        readings = ttk.Frame(live)
        readings.pack(fill="x", pady=3)
        self.live_values = []
        colors = ("#2266ac", "#b44420", "#08766d")
        for rail, color in zip(RAILS, colors):
            var = tk.StringVar(value=f"{rail}   — mV    — mA")
            self.live_values.append(var)
            ttk.Label(readings, textvariable=var, foreground=color,
                      font=("Microsoft YaHei UI", 11, "bold")).pack(side="left", expand=True)
        plots = ttk.Frame(live)
        plots.pack(fill="both", expand=True)
        self.canvases = []
        for column in range(2):
            canvas = tk.Canvas(plots, background="#f6f8fa", highlightthickness=0, width=350, height=140)
            canvas.grid(row=0, column=column, sticky="nsew", padx=3)
            plots.columnconfigure(column, weight=1)
            self.canvases.append(canvas)
            canvas.bind("<Configure>", lambda _: self.draw_live())
        plots.rowconfigure(0, weight=1)
        columns = ("kind", "cycle", "point", "sample", "rail", "target", "ad", "diff", "current", "result")
        self.table = ttk.Treeview(results, columns=columns, show="headings", height=5)
        titles = ("类型", "轮次", "档", "采样", "轨", "目标mV", "AD mV", "偏差mV", "PAC mA", "结果 / RK介质")
        for key, title, width in zip(columns, titles, (90, 55, 45, 45, 65, 95, 95, 95, 95, 240)):
            self.table.heading(key, text=title)
            self.table.column(key, width=width, anchor="center")
        self.table.pack(side="left", fill="both", expand=True)
        scrollbar = ttk.Scrollbar(results, command=self.table.yview)
        scrollbar.pack(side="right", fill="y")
        self.table.configure(yscrollcommand=scrollbar.set)
        self.log = tk.Text(logs, height=5, wrap="word", background="#f5f7f9", state="disabled")
        self.log.pack(fill="both", expand=True)

    def fill_nominal(self):
        for var, value in zip(self.targets, nominal(self.memory_type.get())):
            var.set(str(value))

    def add_point(self):
        try:
            point = VoltagePoint(tuple(int(v.get()) for v in self.targets),
                                 tuple(float(v.get()) if v.get().strip() else None for v in self.references_v),
                                 tuple(float(v.get()) if v.get().strip() else None for v in self.references_i))
        except ValueError as exc:
            messagebox.showerror("电压清单", str(exc), parent=self.root)
            return
        display = lambda values: " / ".join("—" if v is None else f"{v:g}" for v in values)
        key = self.plan.insert("", "end", values=(*point.mv, display(point.reference_mv), display(point.reference_ma)))
        self.points[key] = point

    def remove_point(self):
        for key in self.plan.selection():
            self.plan.delete(key)
            self.points.pop(key, None)

    def clear_points(self):
        self.plan.delete(*self.plan.get_children())
        self.points.clear()

    @staticmethod
    def number(var, name, low, high):
        try:
            value = int(var.get())
        except ValueError:
            raise ValueError(f"{name}须为整数") from None
        if not low <= value <= high:
            raise ValueError(f"{name}范围为{low}～{high}")
        return value

    def update_controls(self):
        connected = self.client.sock is not None
        for widget, state in self.controls:
            widget.configure(state="disabled" if self.busy else state)
        for widget, needs_connection in self.actions:
            widget.configure(state="disabled" if self.busy or (needs_connection and not connected) else "normal")
        self.connect_button.configure(state="disabled" if self.busy or connected else "normal")
        self.stop_button.configure(state="normal" if self.busy else "disabled")
        self.connected_text.set(f"已连接 {self.endpoint}" if connected else "未连接")

    def wire_event(self, event):
        if self.journal:
            self.journal.event(event)
        self.events.put(("log", f"{event['direction']} 0x{event['command']:04X} Seq={event['seq']} " +
                         json.dumps(event["body"], ensure_ascii=False, separators=(",", ":"))))

    def record_row(self, row):
        self.events.put(("row", self.journal.row(row)))

    def runner(self):
        return Runner(self.client, self.stop_event,
                      event=lambda value: self.events.put(("status", value)), row=self.record_row,
                      sample=lambda value: self.events.put(("sample", (time.monotonic(), value))))

    def job(self, name, operation, configuration=None):
        if self.busy:
            return
        self.busy = True
        self.stop_event.clear()
        self.status.set(f"正在{name}……")
        self.update_controls()
        config = {"操作": name, "endpoint": self.endpoint, **(configuration or {})}

        def work():
            try:
                self.journal = Journal(self.report_root, config)
                self.events.put(("report", self.journal.path))
                result = operation()
                self.journal.event({"outcome": result or "完成"})
                self.events.put(("status", str(result or f"{name}完成")))
            except Exception as exc:
                detail = f"{name}停止：{exc}。未自动恢复，电源可能仍ON。"
                self.events.put(("status", detail))
                if self.journal:
                    try:
                        self.journal.event({"error": str(exc), "powerState": "未确认"})
                    except OSError:
                        self.events.put(("status", detail + " 日志写入也失败，请检查磁盘。"))
            finally:
                try:
                    if self.journal:
                        self.journal.close()
                finally:
                    self.journal = None
                    self.events.put(("done", None))

        threading.Thread(target=work, name="B1验板TCP", daemon=True).start()

    def connect(self):
        try:
            port = self.number(self.port, "端口", 1, 65535)
            host = self.host.get().strip()
            if not host:
                raise ValueError("请填写MCU IP")
        except ValueError as exc:
            messagebox.showerror("连接", str(exc), parent=self.root)
            return

        self.endpoint = f"{host}:{port}"

        def operation():
            try:
                self.client.connect(host, port)
                self.client.call(2)
                info = self.client.call(0x0101)
                state = self.client.call(0x0503)
                return f"已连接：{info.get('testerName', '')}；电源{state.get('powerState', '未知')}"
            except Exception:
                self.client.close()
                raise

        self.job("连接和只读查询", operation)

    def query(self, command):
        if command == 0x0601:
            self.output.select(self.live_panel)
            self.job("单次读取", lambda: self.runner().read_monitor(kind="单次读取"))
            return
        self.job(f"0x{command:04X}", lambda: self.client.call(command))

    def manual(self, action):
        try:
            memory_type, mode = self.memory_type.get(), self.boot_mode.get()
            point = VoltagePoint(tuple(int(v.get()) for v in self.targets)) if action == "应用三轨电压" else None
            nominal(memory_type)
        except ValueError as exc:
            messagebox.showerror("单步设置", str(exc), parent=self.root)
            return
        runner = self.runner()
        operations = {"上电": runner.power_on, "下电": runner.power_off,
                      "设置Boot": lambda: runner.set_boot(mode),
                      "应用三轨电压": lambda: runner.set_voltage(point, memory_type)}
        configuration = {"memory_type": memory_type, "boot_mode": mode}
        if point:
            configuration["requested_mv"] = point.mv
        self.job("单步" + action, operations[action], configuration)

    def start_monitor(self):
        if self.busy:
            return
        try:
            interval = self.number(self.monitor_ms, "持续查询间隔", 100, 60000)
        except ValueError as exc:
            messagebox.showerror("持续监测", str(exc), parent=self.root)
            return
        self.live_history.clear()
        self.last_sample = None
        self.live_summary.set("等待第一条采样应答；仅查询，不更改电源。")
        for var, rail in zip(self.live_values, RAILS):
            var.set(f"{rail}   — mV    — mA")
        self.draw_live()
        self.output.select(self.live_panel)
        if self.settings_visible:
            self.toggle_settings()
        runner = self.runner()
        self.job("持续监测", lambda: execute_run(runner, lambda: runner.monitor_loop(interval), False),
                 {"interval_ms": interval, "readonly": True})

    def toggle_settings(self):
        self.settings_visible = not self.settings_visible
        if self.settings_visible:
            self.book.pack(before=self.footer, fill="x")
        else:
            self.book.pack_forget()
        self.settings_button.configure(text="收起设置" if self.settings_visible else "展开设置")

    def start_voltage(self):
        try:
            points = [self.points[key] for key in self.plan.get_children()]
            if not points:
                raise ValueError("请至少添加一档电压")
            parameters = dict(points=points,
                              cycles=self.number(self.volt_cycles, "循环次数", 1, 100000),
                              memory_type=self.memory_type.get(), boot_mode=self.boot_mode.get(),
                              settle_ms=self.number(self.settle_ms, "稳定等待", 0, 600000),
                              samples=self.number(self.samples, "采样数", 1, 10000),
                              interval_ms=self.number(self.interval_ms, "查询间隔", 100, 60000))
        except ValueError as exc:
            messagebox.showerror("调压循环", str(exc), parent=self.root)
            return
        self.start_loop("调压循环", parameters)

    def start_boot(self):
        try:
            parameters = dict(mode=self.boot_mode.get(), memory_type=self.memory_type.get(),
                              cycles=self.number(self.boot_cycles, "循环次数", 1, 100000),
                              off_ms=self.number(self.off_ms, "下电等待", 0, 600000),
                              on_ms=self.number(self.on_ms, "观察时间", 500, 600000))
        except ValueError as exc:
            messagebox.showerror("Boot循环", str(exc), parent=self.root)
            return
        self.start_loop("Boot循环", parameters)

    def start_loop(self, name, parameters):
        self.table.delete(*self.table.get_children())
        self.output.select(self.results_panel)
        power_off = self.power_off_at_end.get()
        saved = {**parameters, "power_off_at_end": power_off}
        if "points" in saved:
            saved["points"] = [asdict(p) for p in saved["points"]]
        runner = self.runner()
        operation = runner.voltage_loop if name == "调压循环" else runner.boot_loop
        self.job(name, lambda: execute_run(runner, lambda: operation(**parameters), power_off), saved)

    def stop(self):
        self.stop_event.set()
        self.status.set("正在停止：先收完已发送命令的应答；持续监测不会下电。")

    def open_report(self):
        path = self.last_report or self.report_root
        path.mkdir(parents=True, exist_ok=True)
        os.startfile(path)

    def close_window(self):
        if self.busy:
            self.closing = True
            self.stop()
        else:
            self.client.close()
            self.root.destroy()

    def show_sample(self, received, view):
        self.last_sample = (received, view)
        self.live_expired = False
        self.live_history.append((received, view))
        self.refresh_live()
        self.draw_live()

    def refresh_live(self):
        """等待应答或停止监测期间仍更新年龄，不把屏幕上的旧数当作当前实测。"""
        if self.last_sample is None:
            return
        received, view = self.last_sample
        age = view["ageMs"]
        if age is not None:
            age += max(0, int((time.monotonic() - received) * 1000))
        expired = age is not None and age > 1500
        description = "停止更新/数据过期" if expired else view["voltageMvStatus"] + "；" + view["currentMaStatus"]
        self.live_summary.set(f"电源：{view['powerState']}（采样时）  |  快照年龄：{age if age is not None else '未知'} ms  |  {description}")
        for i, (var, rail) in enumerate(zip(self.live_values, RAILS)):
            values = [None, None] if expired else [view["voltageMv"][i], view["currentMa"][i]]
            texts = ["—" if v is None else f"{v:g}" for v in values]
            var.set(f"{rail}   {texts[0]} mV    {texts[1]} mA")
        # 失去更新时插入断点，恢复后曲线不跨过这段空白连接。
        if expired and not self.live_expired:
            self.live_history.append((time.monotonic(), {"voltageMv": [None] * 3, "currentMa": [None] * 3}))
            self.live_expired = True
            self.draw_live()

    def draw_live(self):
        for canvas, field, unit in zip(self.canvases, ("voltageMv", "currentMa"), ("电压 mV", "电流 mA")):
            canvas.delete("all")
            width, height = canvas.winfo_width(), canvas.winfo_height()
            if width < 160 or height < 75:
                canvas.create_text(width / 2, height / 2, text="收起设置区可展开曲线", fill="#687782")
                continue
            left, top, right, bottom = 65, 24, width - 15, height - 26
            canvas.create_text(8, 10, text=unit, anchor="w", fill="#364550")
            canvas.create_line(left, top, left, bottom, right, bottom, fill="#aebbc4")
            values = [v for _, row in self.live_history for v in row[field] if v is not None]
            if not values:
                canvas.create_text((left + right) / 2, (top + bottom) / 2, text="暂无有效数据", fill="#687782")
                continue
            low, high = min(values), max(values)
            pad = max((high - low) * 0.1, abs(high) * 0.01, 1)
            low, high = low - pad, high + pad
            start, end = self.live_history[0][0], self.live_history[-1][0]
            span = max(end - start, 0.001)
            for n in range(3):
                y = bottom - (bottom - top) * n / 2
                canvas.create_line(left, y, right, y, fill="#e0e6eb")
                canvas.create_text(left - 6, y, text=f"{low + (high - low) * n / 2:.3g}", anchor="e", fill="#52616c")
            canvas.create_text(right, height - 9, text=f"最近 {end - start:.1f} 秒 / 最多300次查询", anchor="e", fill="#52616c")
            for rail, color in enumerate(("#2266ac", "#b44420", "#08766d")):
                previous = None
                for timestamp, row in self.live_history:
                    value = row[field][rail]
                    if value is None:
                        previous = None
                        continue
                    x = left + (right - left) * (timestamp - start) / span
                    y = bottom - (bottom - top) * (value - low) / (high - low)
                    if previous:
                        canvas.create_line(*previous, x, y, fill=color, width=2)
                    else:
                        canvas.create_oval(x - 2, y - 2, x + 2, y + 2, fill=color, outline=color)
                    previous = (x, y)

    def drain(self):
        for _ in range(200):
            try:
                kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            if kind in ("log", "status"):
                if kind == "status":
                    self.status.set(value)
                self.log.configure(state="normal")
                self.log.insert("end", f"{datetime.now():%H:%M:%S} {value[:1500]}\n")
                if int(self.log.index("end-1c").split(".")[0]) > 700:
                    self.log.delete("1.0", "150.0")
                self.log.see("end")
                self.log.configure(state="disabled")
            elif kind == "report":
                self.last_report = value
                self.report_text.set(f"本次记录：{value}")
            elif kind == "row":
                keys = ("类型", "轮次", "电压点", "采样序号", "电源轨", "目标mV", "AD实测mV",
                        "AD减目标mV", "PAC实测mA")
                end = value.get("RK实际启动介质", value.get("说明", "仅记录偏差"))
                key = self.table.insert("", "end", values=(*(value.get(k, "") for k in keys), end))
                if len(self.table.get_children()) > 500:
                    self.table.delete(self.table.get_children()[0])
                self.table.see(key)
            elif kind == "sample":
                self.show_sample(*value)
            elif kind == "done":
                self.busy = False
                self.update_controls()
                if self.closing:
                    self.client.close()
                    self.root.destroy()
                    return
        self.refresh_live()
        self.root.after(80, self.drain)


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
