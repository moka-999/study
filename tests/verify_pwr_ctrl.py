#!/usr/bin/env python3
"""验证 pwr_ctrl 的源码合同，并用纯 Python 模型覆盖关键状态迁移。"""

from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class State(IntEnum):
    OFF = 0
    WAIT_FIRST = 1
    WAIT_SECOND = 2
    WAIT_DDR = 3
    ON = 4
    WAIT_DDR_OFF = 5
    WAIT_SECOND_OFF = 6
    FAULT = 7


@dataclass(frozen=True)
class Config:
    """仅用于离线模型的时间参数，不代表 B1 真板批准值。"""

    four_volt_first: bool = True
    first_to_second_ms: int = 5
    second_to_ddr_ms: int = 7
    ddr_to_config_ms: int = 3
    ddr_to_system_off_ms: int = 4
    system_off_spacing_ms: int = 5


class Model:
    """记录虚拟 GPIO 事件，复刻 C 状态机的可观察行为。"""

    def __init__(self, config: Config) -> None:
        self.config = config
        self.state = State.OFF
        self.enter_ms = 0
        self.outputs = {"4V0": 0, "3V3": 0, "DDR": 0}
        self.events: list[tuple[int, str, int]] = []

    @property
    def first(self) -> str:
        return "4V0" if self.config.four_volt_first else "3V3"

    @property
    def second(self) -> str:
        return "3V3" if self.config.four_volt_first else "4V0"

    def set_output(self, now_ms: int, rail: str, level: int) -> None:
        self.outputs[rail] = level
        self.events.append((now_ms, rail, level))

    def enter(self, now_ms: int, state: State) -> None:
        self.state = state
        self.enter_ms = now_ms

    def request_on(self, now_ms: int) -> None:
        assert self.state == State.OFF
        self.set_output(now_ms, self.first, 1)
        self.enter(now_ms, State.WAIT_FIRST)

    def request_off(self, now_ms: int) -> None:
        if self.state != State.ON:
            for rail in ("DDR", "3V3", "4V0"):
                self.set_output(now_ms, rail, 0)
            self.enter(now_ms, State.OFF)
            return
        self.set_output(now_ms, "DDR", 0)
        self.enter(now_ms, State.WAIT_DDR_OFF)

    def process(self, now_ms: int, ddr_ok: bool = True) -> None:
        elapsed = now_ms - self.enter_ms
        if self.state == State.WAIT_FIRST and elapsed >= self.config.first_to_second_ms:
            self.set_output(now_ms, self.second, 1)
            self.enter(now_ms, State.WAIT_SECOND)
        elif self.state == State.WAIT_SECOND and elapsed >= self.config.second_to_ddr_ms:
            self.set_output(now_ms, "DDR", 1)
            self.enter(now_ms, State.WAIT_DDR)
        elif self.state == State.WAIT_DDR and elapsed >= self.config.ddr_to_config_ms:
            if ddr_ok:
                self.enter(now_ms, State.ON)
            else:
                for rail in ("DDR", "3V3", "4V0"):
                    self.set_output(now_ms, rail, 0)
                self.enter(now_ms, State.FAULT)
        elif self.state == State.WAIT_DDR_OFF and elapsed >= self.config.ddr_to_system_off_ms:
            self.set_output(now_ms, self.second, 0)
            self.enter(now_ms, State.WAIT_SECOND_OFF)
        elif self.state == State.WAIT_SECOND_OFF and elapsed >= self.config.system_off_spacing_ms:
            self.set_output(now_ms, self.first, 0)
            self.enter(now_ms, State.OFF)


def verify_source_contract() -> None:
    source = (ROOT / "HAL_Drv/pwr_ctrl.c").read_text(encoding="utf-8")
    header = (ROOT / "HAL_Drv/pwr_ctrl.h").read_text(encoding="utf-8")
    main = (ROOT / "Core/Src/main.c").read_text(encoding="utf-8")
    freertos = (ROOT / "Core/Src/freertos.c").read_text(encoding="utf-8")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

    assert "pwr_ctrl_init(NULL);" in main, "启动时必须保持未配置和默认关闭"
    assert "pwr_ctrl_process(HAL_GetTick());" in freertos, "状态机缺少周期推进入口"
    assert "ddr_power_init()" in source, "PG11 使能后必须配置 DDR 三轨"
    assert "force_all_rails_off();" in source, "故障路径缺少统一关断"
    assert "vTaskDelay(" not in source and "osDelay(" not in source, "轨间等待禁止使用任务延时"
    assert "PWR_CTRL_ERR_NOT_CONFIGURED" in header, "未确认时序必须拒绝上电"
    assert "HAL_Drv/pwr_ctrl.c" in cmake, "pwr_ctrl 未加入固件构建"


def verify_wave_image_contract() -> None:
    """确认波形测试与正式构建隔离，且绝不会误用于 B1 真板。"""

    source = (ROOT / "HAL_Drv/pwr_ctrl.c").read_text(encoding="utf-8")
    task = (ROOT / "App/tasks/pwr_wave_task.c").read_text(encoding="utf-8")
    freertos = (ROOT / "Core/Src/freertos.c").read_text(encoding="utf-8")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    dev_script = (ROOT / "tools/dev.ps1").read_text(encoding="utf-8")

    assert 'option(ENABLE_PWR_WAVE_TEST "Build Apollo V2 pwr_ctrl waveform image" OFF)' in cmake
    assert "ENABLE_PWR_WAVE_TEST=1" in cmake, "波形镜像缺少独立编译宏"
    assert "StartPwrWaveTask" in freertos, "FreeRTOS 未创建波形测试任务"
    assert "ENABLE_PWR_WAVE_TEST 只允许用于阿波罗 V2" in source, "缺少 B1 编译期保护"
    assert "ddr_power_init()" in source, "正式构建不得移除真实 DDR 初始化"
    assert "PWR_WAVE_STEP_MS       500U" in task, "逻辑分析仪测试步长应清晰固定"
    assert "build_pwr_wave" in dev_script, "波形镜像必须使用独立构建目录"


def verify_four_volt_first_sequence() -> None:
    model = Model(Config())
    model.request_on(0)
    model.process(4)
    assert model.events == [(0, "4V0", 1)]
    model.process(5)
    model.process(12)
    model.process(15)
    assert model.state == State.ON
    assert model.events == [(0, "4V0", 1), (5, "3V3", 1), (12, "DDR", 1)]

    model.request_off(20)
    model.process(24)
    model.process(29)
    assert model.state == State.OFF
    assert model.events[-3:] == [(20, "DDR", 0), (24, "3V3", 0), (29, "4V0", 0)]


def verify_alternate_order_and_fault() -> None:
    alternate = Model(Config(four_volt_first=False))
    alternate.request_on(100)
    alternate.process(105)
    assert alternate.events[:2] == [(100, "3V3", 1), (105, "4V0", 1)]

    fault = Model(Config())
    fault.request_on(0)
    fault.process(5)
    fault.process(12)
    fault.process(15, ddr_ok=False)
    assert fault.state == State.FAULT
    assert fault.outputs == {"4V0": 0, "3V3": 0, "DDR": 0}


def verify_wave_image_timeline() -> None:
    """验证文档中给逻辑分析仪使用的500ms循环时间线。"""

    wave = Model(Config(
        first_to_second_ms=500,
        second_to_ddr_ms=500,
        ddr_to_config_ms=500,
        ddr_to_system_off_ms=500,
        system_off_spacing_ms=500,
    ))
    wave.request_on(0)
    wave.process(500)
    wave.process(1000)
    wave.process(1500)
    assert wave.state == State.ON

    wave.request_off(2500)
    wave.process(3000)
    wave.process(3500)
    assert wave.state == State.OFF
    assert wave.events == [
        (0, "4V0", 1),
        (500, "3V3", 1),
        (1000, "DDR", 1),
        (2500, "DDR", 0),
        (3000, "3V3", 0),
        (3500, "4V0", 0),
    ]


def main() -> None:
    verify_source_contract()
    verify_wave_image_contract()
    verify_four_volt_first_sequence()
    verify_alternate_order_and_fault()
    verify_wave_image_timeline()
    print("pwr_ctrl: production/wave-image contracts and 4 state-machine scenarios PASS")
    print("Timing values are test-only; B1 PC6/PC7 order remains explicitly unconfigured")


if __name__ == "__main__":
    main()
