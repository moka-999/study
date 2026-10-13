"""用原GNU/QEMU运行UART路由、RK帧和IDD统计正式C代码；不连接板卡。"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--qemu", default="D:/qemu/qemu-system-arm.exe")
    parser.add_argument("--log-default", choices=(0, 1), type=int, default=0,
                        help="仅B1控制台回归：0=MCU默认，1=RK默认")
    args = parser.parse_args()
    root = args.app_root.resolve()
    boot = root.parent / "bootloader"
    is_b1 = (root / "b1.ioc").exists()
    build = root / ("build/uart-idd-test-rk-default" if is_b1 and args.log_default else "build/uart-idd-test")
    build.mkdir(parents=True, exist_ok=True)
    # 仅用队列/HAL声明替换硬件和调度器；被测路由、状态机、帧及统计原样链接。
    (build / "test_port.h").write_text('''#pragma once
#include <stdint.h>
typedef struct { uint32_t unused; } GPIO_TypeDef;
typedef struct { uint32_t ErrorCode; int event; } UART_HandleTypeDef;
typedef uint32_t TickType_t;
typedef unsigned UBaseType_t;
typedef int BaseType_t;
typedef void *QueueHandle_t;
#define pdFALSE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define HAL_UART_RXEVENT_HT 1
#define HAL_UART_RXEVENT_IDLE 2
#define HAL_UART_RXEVENT_TC 3
void test_enter(void);
void test_exit(void);
#define taskENTER_CRITICAL() test_enter()
#define taskEXIT_CRITICAL() test_exit()
#define portYIELD_FROM_ISR(x) ((void)(x))
TickType_t xTaskGetTickCount(void);
QueueHandle_t xQueueCreate(UBaseType_t, UBaseType_t);
BaseType_t xQueueSendFromISR(QueueHandle_t, const void *, BaseType_t *);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueReset(QueueHandle_t);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t);
int HAL_UARTEx_GetRxEventType(UART_HandleTypeDef *);
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *, uint16_t);
void HAL_UART_ErrorCallback(UART_HandleTypeDef *);
uint32_t __get_IPSR(void);
''', encoding="utf-8")
    for name in ("main", "FreeRTOS", "task", "queue"):
        (build / f"{name}.h").write_text('#include "test_port.h"\n', encoding="utf-8")
    includes = [build] + [root / p for p in ("HAL_Drv", "BSP/AD7124", "BSP/PAC1934", "App/protocol/rk")]
    sources = [Path(__file__).with_name("test_uart_idd.c")] + [root / p for p in (
        "HAL_Drv/uart_router.c", "HAL_Drv/idd_stats.c", "App/protocol/rk/frame.c",
        "App/protocol/rk/crc.c", "App/protocol/rk/state_machine.c")] + [boot / "tests/qemu_start.s"]
    if is_b1:
        sources.append(root / "Core/Src/log_console.c")
        includes.append(root / "Core/Inc")
    command = [args.compiler, "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
               "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-ffunction-sections",
               "-fdata-sections", *["-I" + str(p) for p in includes], *map(str, sources),
               "-nostartfiles", "--specs=nosys.specs", "-T" + str(boot / "tests/qemu_test.ld"),
               "-Wl,--gc-sections", "-o", str(build / "test.elf")]
    if is_b1:
        command += ["-DTEST_B1_LOG_CONSOLE=1", f"-DB1_LOG_DEFAULT_SOURCE={args.log_default}"]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (build / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run([args.qemu, "-M", "mps2-an500", "-display", "none", "-monitor", "none",
                             "-serial", "none", "-no-reboot", "-semihosting-config", "enable=on,target=native",
                             "-kernel", str(build / "test.elf")], capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / "run.log").write_text(output, encoding="utf-8")
    if result.returncode or "PASS: UART recovery, RK bounds and IDD windows" not in output:
        raise RuntimeError(output)
    if is_b1 and "PASS: B1 selectable log console" not in output:
        raise RuntimeError("B1日志切换用例未实际执行")
    print(f"{root.name}: {output.strip()}")


if __name__ == "__main__":
    main()
