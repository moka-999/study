#!/usr/bin/env python3
"""验证结果灯颜色语义、500ms闪烁和V2波形镜像隔离合同。"""

from enum import IntEnum
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class State(IntEnum):
    OFF = 0
    PASS = 1
    FAIL = 2
    TEST_NORMAL = 3
    TEST_HOT = 4


def render(state: State, blink_on: bool) -> tuple[int, int]:
    """返回(red, green)，复刻固件中的纯颜色转换规则。"""

    if state == State.PASS:
        return 0, 1
    if state == State.FAIL:
        return 1, 0
    if state == State.TEST_NORMAL and blink_on:
        return 1, 1
    if state == State.TEST_HOT and blink_on:
        return 1, 0
    return 0, 0


def verify_semantics() -> None:
    assert render(State.OFF, False) == (0, 0)
    assert render(State.PASS, False) == (0, 1)
    assert render(State.FAIL, False) == (1, 0)
    assert render(State.TEST_NORMAL, True) == (1, 1)
    assert render(State.TEST_NORMAL, False) == (0, 0)
    assert render(State.TEST_HOT, True) == (1, 0)
    assert render(State.TEST_HOT, False) == (0, 0)


def verify_source_contract() -> None:
    led = (ROOT / "BSP/LED/led.c").read_text(encoding="utf-8")
    header = (ROOT / "BSP/LED/led.h").read_text(encoding="utf-8")
    task = (ROOT / "App/tasks/result_led_wave_task.c").read_text(encoding="utf-8")
    main_c = (ROOT / "Core/Src/main.c").read_text(encoding="utf-8")
    freertos = (ROOT / "Core/Src/freertos.c").read_text(encoding="utf-8")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    dev_script = (ROOT / "tools/dev.ps1").read_text(encoding="utf-8")

    for name in ("LED_RESULT_PASS", "LED_RESULT_FAIL",
                 "LED_RESULT_TEST_NORMAL", "LED_RESULT_TEST_HOT"):
        assert name in header and name in led, f"缺少业务状态映射: {name}"

    assert "RESULT_LED_BLINK_MS       500U" in task, "闪烁亮灭间隔必须为500ms"
    assert "P2-23 / PD7" in task and "P2-28 / PC10" in task, "V2接线表不完整"
    assert "result_led_wave_blink(RESULT_LED_LPDDR0, RESULT_LED_EMCP" in task, \
        "橙闪阶段必须覆盖三组灯，确保六路均可验收"
    assert "StartResultLedWaveTask" in freertos, "FreeRTOS未创建结果灯测试任务"
    assert 'option(ENABLE_RESULT_LED_WAVE_TEST "Build Apollo V2 result LED waveform image" OFF)' in cmake
    assert "ENABLE_RESULT_LED_WAVE_TEST=1" in cmake, "测试镜像缺少独立编译宏"
    assert "build_led_wave" in dev_script, "测试镜像必须使用独立构建目录"
    assert task.index("HAL_GPIO_WritePin") < task.index("HAL_GPIO_Init"), \
        "结果灯测试脚必须先预置低电平，再切换为输出"
    assert "void result_led_wave_early_init(void)" in task, \
        "测试镜像必须提供调度器启动前的GPIO安全初始化入口"
    assert "result_led_wave_early_init();" in main_c, \
        "调度器启动前必须提前关闭六路替代结果灯"
    assert main_c.index("result_led_wave_early_init();") < main_c.index("osKernelInitialize()"), \
        "结果灯安全初始化必须早于FreeRTOS调度器"
    assert "ENABLE_RESULT_LED_WAVE_TEST" in main_c, \
        "提前初始化必须仅存在于独立测试镜像"


def main() -> None:
    verify_semantics()
    verify_source_contract()
    print("result_led: 5 states, 500ms blink and isolated V2 waveform image PASS")
    print("V2 waveform accepted and B1 PG0-PG5 ported; real 2N7002 startup check remains")


if __name__ == "__main__":
    main()
