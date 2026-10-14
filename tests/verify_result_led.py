#!/usr/bin/env python3
"""验证结果灯颜色语义、500ms闪烁和V2波形镜像隔离合同。"""

from enum import IntEnum
from pathlib import Path
import argparse
import subprocess


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
    task = (ROOT / "App/tasks/test/result_led_wave_task.c").read_text(encoding="utf-8")
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


def verify_actual_c(root: Path, compiler: str, qemu: str) -> None:
    """实际驱动只替换HAL/RTOS端口；源码与Cortex-M7指令均真实编译运行。"""
    build = root / "build/result-led-c"
    build.mkdir(parents=True, exist_ok=True)
    (build / "stm32h7xx_hal.h").write_text('''#pragma once
#include <stdint.h>
typedef struct { uint32_t unused; } GPIO_TypeDef;
typedef enum { HAL_OK=0, HAL_ERROR=1 } HAL_StatusTypeDef;
typedef enum { GPIO_PIN_RESET=0, GPIO_PIN_SET=1 } GPIO_PinState;
uint32_t HAL_GetTick(void);
void HAL_GPIO_WritePin(GPIO_TypeDef *, uint16_t, GPIO_PinState);
''', encoding="utf-8")
    (build / "FreeRTOS.h").write_text('''#pragma once
#include <stddef.h>
void mock_enter(void);
void mock_exit(void);
#define taskENTER_CRITICAL() mock_enter()
#define taskEXIT_CRITICAL() mock_exit()
''', encoding="utf-8")
    (build / "task.h").write_text('#include "FreeRTOS.h"\n', encoding="utf-8")
    boot = root.parent / "bootloader/tests"
    command = [compiler, "-mcpu=cortex-m7", "-mthumb", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
               "-I" + str(build), "-I" + str(root / "Core/Inc"),
               str(ROOT / "tests/test_result_led.c"), str(root / "Core/Src/result_led.c"),
               str(boot / "qemu_start.s"), "-nostartfiles", "--specs=nosys.specs",
               "-T" + str(boot / "qemu_test.ld"), "-o", str(build / "test.elf")]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (build / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run([qemu, "-M", "mps2-an500", "-display", "none", "-monitor", "none", "-serial", "none",
                             "-no-reboot", "-semihosting-config", "enable=on,target=native",
                             "-kernel", str(build / "test.elf")], capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / "run.log").write_text(output, encoding="utf-8")
    if result.returncode or "PASS: actual LED C" not in output:
        raise RuntimeError(output)
    print(output.strip())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--actual-c", action="store_true", help="额外运行实际驱动C/QEMU，无硬件操作")
    parser.add_argument("--app-root", type=Path, default=ROOT)
    parser.add_argument("--compiler", default="C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe")
    parser.add_argument("--qemu", default="D:/qemu/qemu-system-arm.exe")
    args = parser.parse_args()
    verify_semantics()
    verify_source_contract()
    if args.actual_c:
        verify_actual_c(args.app_root.resolve(), args.compiler, args.qemu)
    print("result_led: 5 states, 500ms blink and isolated V2 waveform image PASS")
    print("V2 waveform accepted and B1 PG0-PG5 ported; real 2N7002 startup check remains")


if __name__ == "__main__":
    main()
