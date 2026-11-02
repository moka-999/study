#!/usr/bin/env python3
"""运行实际PC分发、RK业务和队列接口的C回归；模拟平台端口，不连接任何设备。"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--compiler", default="C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe")
    parser.add_argument("--qemu", default="D:/qemu/qemu-system-arm.exe")
    parser.add_argument("--b1-guard", action="store_true", help="核对B1真实ON门禁及实际采样统计接入")
    parser.add_argument("--power-simulation", action="store_true", help="验证V2部分模拟仍通过真实RK队列及正式分发")
    parser.add_argument("--mats-fail-test", action="store_true", help="仅V2电源模拟：验证每次MATS最终回包强制FAIL")
    parser.add_argument("--power-sequence", action="store_true", help="执行正式电源编排与0103的故障注入，不访问设备")
    parser.add_argument("--round", action="store_true", help="验证整轮、SLT收尾和070x删除；只运行离线实际C")
    args = parser.parse_args()
    root = args.app_root.resolve()
    if args.power_simulation and (args.b1_guard or root.name == "b1"):
        parser.error("B1禁止电源模拟")
    if args.mats_fail_test and not args.power_simulation:
        parser.error("MATS失败联调必须显式开启V2电源模拟")
    if args.power_sequence and args.power_simulation:
        parser.error("真实电源编排回归不启用V2模拟实现")
    suffix = "-power-sim" if args.power_simulation else "-b1-guard" if args.b1_guard else ""
    if args.mats_fail_test:
        suffix += "-mats-fail"
    if args.power_sequence:
        suffix += "-power-sequence"
    if args.round:
        suffix += "-round"
    build = root / "build" / ("pc-rk-service" + suffix)
    build.mkdir(parents=True, exist_ok=True)
    # 只模拟RTOS队列和HAL类型，不替换命令表、JSON解析、服务生命周期或响应组包。
    (build / "port.h").write_text('''#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef void *QueueHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define configTICK_RATE_HZ 1000U
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
int xQueueSend(QueueHandle_t, const void *, uint32_t);
int xQueueReceive(QueueHandle_t, void *, uint32_t);
''', encoding="utf-8")
    for name in ("FreeRTOS", "queue", "task"):
        (build / (name + ".h")).write_text('#include "port.h"\n', encoding="utf-8")
    (build / "stm32h7xx_hal.h").write_text('''#pragma once
#include <stdint.h>
typedef struct { uint32_t unused; } GPIO_TypeDef;
typedef enum { HAL_OK=0, HAL_ERROR=1 } HAL_StatusTypeDef;
uint32_t HAL_GetTick(void);
''', encoding="utf-8")
    (build / "main.h").write_text('#include "stm32h7xx_hal.h"\n', encoding="utf-8")
    if args.b1_guard and root.name != "b1":
        # V2源码检查B1条件分支时只补日志端口；真实B1构建仍使用自身正式接口头。
        (build / "log_console.h").write_text(
            '#include <stddef.h>\nsize_t log_console_write(const char *, size_t);\n', encoding="utf-8")
    sources = [Path(__file__).with_name("test_pc_rk_service.c"), *[root / path for path in (
        "App/protocol/pc/pc_dispatch.c", "App/protocol/pc/pc_test_service.c",
        "App/protocol/pc/pc_power_service.c",
        "App/protocol/pc/pc_protocol.c", "App/protocol/pc/pc_test_message.c",
        "App/tasks/rk/rk_link.c", "HAL_Drv/ddr_power.c", "Middlewares/Third_Party/cJSON/cJSON.c")]]
    if args.power_simulation or args.b1_guard or root.name == "b1":
        sources.append(root / "HAL_Drv/idd_stats.c")
    if args.power_simulation:
        sources += [root / path for path in ("App/tasks/test/v2_power_sim.c", "BSP/MCP4725/mcp4725.c")]
    includes = [build, *[root / path for path in (
        "App/protocol/pc", "App/protocol/rk", "App/protocol/b2", "App/tasks/b2",
        "App/tasks/rk", "App/tasks/common", "App/tasks/test", "HAL_Drv",
        "BSP/TPS62868", "BSP/PAC1934", "BSP/AD7124", "BSP/MCP4725", "BSP/Flash",
        "Middlewares/Third_Party/cJSON", "Core/Inc")]]
    support = Path(__file__).resolve().parent / "support"
    command = [args.compiler, "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
               "-std=c11", "-O1", "-g", "-DCJSON_NESTING_LIMIT=16",
               "-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections"]
    if args.b1_guard or root.name == "b1":
        command += ["-DB1_RK_UART=4"]
    if args.power_simulation:
        command += ["-DENABLE_V2_POWER_SIMULATION=1"]
    if args.mats_fail_test:
        command += ["-DENABLE_V2_MATS_FAIL_TEST=1"]
    if args.power_sequence:
        command += ["-DPC_POWER_SEQUENCE_TEST=1"]
    if args.round:
        command += ["-DPC_TEST_ROUND_TEST=1"]
    command += ["-I" + str(path) for path in includes]
    command += [*map(str, sources), str(support / "qemu_start.s"), "-nostartfiles", "--specs=nosys.specs",
                "-T" + str(support / "qemu_test.ld"), "-Wl,--gc-sections", "-o", str(build / "test.elf")]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (build / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run([args.qemu, "-M", "mps2-an500", "-display", "none", "-monitor", "none",
                             "-serial", "none", "-no-reboot", "-semihosting-config", "enable=on,target=native",
                             "-kernel", str(build / "test.elf")], capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / "run.log").write_text(output, encoding="utf-8")
    if result.returncode or "PASS: PC RK service" not in output:
        raise RuntimeError(output)
    print(output.strip())
    print(f"Target: {root}; scope: actual C under QEMU, no hardware/network validation")


if __name__ == "__main__":
    main()
