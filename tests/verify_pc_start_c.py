#!/usr/bin/env python3
"""在QEMU执行0103 JSON合同及旧电源模式拒绝；真实RK业务另见verify_pc_rk_service.py。"""
import argparse
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--boot-root", type=Path, default=Path(__file__).resolve().parents[2] / "bootloader")
    parser.add_argument("--compiler", default=shutil.which("arm-none-eabi-gcc") or "arm-none-eabi-gcc")
    parser.add_argument("--qemu", default=shutil.which("qemu-system-arm") or "D:/qemu/qemu-system-arm.exe")
    parser.add_argument("--expected-rsense", type=int, choices=(10000, 20000))
    args = parser.parse_args()
    root = args.app_root.resolve()
    expected_rsense = args.expected_rsense or (20000 if (root / "b1.ioc").exists() else 10000)
    build = root / "build/pc-start-test"
    build.mkdir(parents=True, exist_ok=True)
    # 只提供编译必需的HAL类型；所有真实业务入口仍使用项目头文件声明。
    (build / "stm32h7xx_hal.h").write_text(
        "#pragma once\n#include <stdint.h>\ntypedef struct { uint32_t unused; } GPIO_TypeDef;\n"
        "typedef enum { HAL_OK=0, HAL_ERROR=1 } HAL_StatusTypeDef;\nuint32_t HAL_GetTick(void);\n",
        encoding="utf-8")
    (build / "main.h").write_text('#include "stm32h7xx_hal.h"\n', encoding="utf-8")
    # 本回归专注解析和旧电源门禁，不代替真实RK服务的独立回归。
    (build / "service_stub.c").write_text('''#include "pc_test_service.h"
#include "ota_app.h"
#include <stdio.h>
#include <string.h>
int pc_test_service_busy(void) { return 0; }
pc_dispatch_result_t pc_test_service_start(const pc_test_start_view_t *start,
    uint32_t sequence, pc_dispatch_response_t *response)
{
    char json[160];
    const char *error = ota_app_is_confirmed() ? "notReady" : "otaNotReady";
    int length = snprintf(json, sizeof(json),
        "{\\"requestCommandId\\":259,\\"algorithmIndex\\":%lu,\\"status\\":\\"failed\\",\\"errorCode\\":\\"%s\\"}",
        (unsigned long)start->algorithm_index, error);
    if (length <= 0 || (size_t)length >= sizeof(json)) return PC_DISPATCH_ERROR;
    return pc_protocol_build_response(PC_COMMAND_COMMAND_RESP, sequence,
        (const uint8_t *)json, (uint32_t)length, response->data, sizeof(response->data),
        &response->length) == 0 ? PC_DISPATCH_RESPONSE_READY : PC_DISPATCH_ERROR;
}
int pc_test_service_poll(uint32_t sequence, pc_dispatch_response_t *response)
{ (void)sequence; (void)response; return 0; }
void pc_test_service_disconnected(void) { }
''', encoding="utf-8")
    sources = [Path(__file__).with_name("test_pc_start.c"), build / "service_stub.c",
               *[root / p for p in ("App/protocol/pc/pc_dispatch.c", "App/protocol/pc/pc_protocol.c",
                                   "App/protocol/pc/pc_state_machine.c", "App/protocol/pc/pc_test_message.c", "App/protocol/pc/pc_power_service.c", "HAL_Drv/ddr_power.c", "BSP/PAC1934/pac1934.c",
                                   "Middlewares/Third_Party/cJSON/cJSON.c")],
               args.boot_root / "tests/qemu_start.s"]
    includes = [build, *[root / p for p in ("App/protocol/pc", "App/tasks/common", "App/ota", "HAL_Drv",
                 "BSP/TPS62868", "BSP/PAC1934", "BSP/AD7124", "BSP/MCP4725", "BSP/Flash",
                 "Middlewares/Third_Party/cJSON", "Core/Inc")]]
    command = [str(args.compiler), "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
               "-g", "-O1", "-DENABLE_OTA_BOOT=1", "-DCJSON_NESTING_LIMIT=16", f"-DPC_TEST_EXPECTED_RSENSE={expected_rsense}",
               "-Wall", "-Wextra", "-Werror",
               "-ffunction-sections", "-fdata-sections", *["-I" + str(path) for path in includes],
               *map(str, sources), "-nostartfiles", "--specs=nosys.specs",
               "-T" + str(args.boot_root / "tests/qemu_test.ld"), "-Wl,--gc-sections", "-o", str(build / "test.elf")]
    if (root / "b1.ioc").exists():
        command.insert(1, "-DB1_RK_UART=4")
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (build / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run([args.qemu, "-M", "mps2-an500", "-display", "none", "-monitor", "none",
                             "-serial", "none", "-no-reboot", "-semihosting-config", "enable=on,target=native",
                             "-kernel", str(build / "test.elf")], capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / "run.log").write_text(output, encoding="utf-8")
    if result.returncode or "PASS: actual START_TEST" not in output:
        raise RuntimeError(output)
    print(output.strip())
    print(f"Target: {root}; scope: real C with rejecting hardware stubs, not board/network validation")


if __name__ == "__main__":
    main()
