#!/usr/bin/env python3
"""在QEMU执行0103 JSON合同及旧电源模式拒绝；真实RK业务另见verify_pc_rk_service.py。"""
import argparse
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-root", type=Path, default=Path(__file__).resolve().parents[1])
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
    service_stub = '''#include "pc_test_service.h"
#include <stdio.h>
#include <string.h>
int pc_test_service_busy(void) { return 0; }
const char *pc_test_service_stop(void) { return "notSupported"; }
/* 本解析回归不会启动控制台电源；实际控制台及0104取消另有业务回归。 */
void log_console_complete_power_request(const char *error) { (void)error; __builtin_trap(); }
pc_dispatch_result_t pc_test_service_start(uint16_t request_command, const pc_test_start_view_t *start,
    uint32_t sequence, pc_dispatch_response_t *response)
{
    char json[160];
    const char *error = "notReady";
    int length = snprintf(json, sizeof(json),
        "{\\"requestCommandId\\":%u,\\"algorithmIndex\\":%lu,\\"status\\":\\"failed\\",\\"errorCode\\":\\"%s\\"}",
        (unsigned)request_command, (unsigned long)start->algorithm_index, error);
    if (length <= 0 || (size_t)length >= sizeof(json)) return PC_DISPATCH_ERROR;
    return pc_protocol_build_response(PC_COMMAND_COMMAND_RESP, sequence,
        (const uint8_t *)json, (uint32_t)length, response->data, sizeof(response->data),
        &response->length) == 0 ? PC_DISPATCH_RESPONSE_READY : PC_DISPATCH_ERROR;
}
int pc_test_service_poll(uint32_t sequence, pc_dispatch_response_t *response)
{ (void)sequence; (void)response; return 0; }
void pc_test_service_disconnected(void) { }
/* 整轮与SLT生命周期由verify_pc_rk_service验证；本测试只替代这些边界。 */
pc_dispatch_result_t pc_test_round_begin(uint32_t seq, pc_dispatch_response_t *out)
{ (void)seq; (void)out; return PC_DISPATCH_ERROR; }
pc_dispatch_result_t pc_test_round_end(uint32_t mask, uint32_t seq, pc_dispatch_response_t *out)
{ (void)mask; (void)seq; (void)out; return PC_DISPATCH_ERROR; }
pc_dispatch_result_t pc_test_slt_start(const pc_test_start_view_t *view, uint32_t seq, pc_dispatch_response_t *out)
{ (void)view; (void)seq; (void)out; return PC_DISPATCH_ERROR; }
'''
    # V2仍保留手动SLT结果入口；B1已移除此接口，只按实际导出的头文件生成替身。
    if 'pc_test_slt_result(' in (root / 'App/protocol/pc/pc_test_service.h').read_text(encoding='utf-8'):
        service_stub += '''pc_dispatch_result_t pc_test_slt_result(const pc_slt_result_view_t *view, uint32_t seq, pc_dispatch_response_t *out)
{ (void)view; (void)seq; (void)out; return PC_DISPATCH_ERROR; }
'''
    if (root / 'App/protocol/pc/pc_site_control.h').is_file():
        service_stub += '''#include "pc_site_control.h"
/* 此套件固定开站，010C的真实分发、停机和恢复由站点专项运行。 */
int pc_site_enabled(void) { return 1; }
const char *pc_site_configure(const pc_site_config_t *config, int *deferred)
{ (void)config; (void)deferred; __builtin_trap(); }
int pc_site_take_reply(const char **error) { (void)error; return 0; }
void pc_site_disconnected(void) { }
'''
    if (root / 'App/protocol/pc/pc_shmoo.h').is_file():
        service_stub += '''/* 此套件只验证普通启动解析；扫描由真实业务回归覆盖。 */
pc_dispatch_result_t pc_test_shmoo_start(const cJSON *root, uint32_t seq, pc_dispatch_response_t *out)
{ (void)root; (void)seq; (void)out; __builtin_trap(); }
void pc_test_shmoo_point_sent(void) { __builtin_trap(); }
'''
    if (root / "b1.ioc").exists():
        service_stub += '''#include "rk_task_control.h"
/* 这组用例仅解析请求；启动 RK 的专项在实际业务回归中验证。 */
void rk_timing_readback_arm(int deferred) { (void)deferred; __builtin_trap(); }
int rk_task_set_pending(void) { return 0; }
void rk_link_manual_frequency(int64_t value) { (void)value; __builtin_trap(); }
'''
    (build / 'service_stub.c').write_text(service_stub, encoding='utf-8')
    sources = [Path(__file__).with_name("test_pc_start.c"), build / "service_stub.c",
               *[root / p for p in ("App/protocol/pc/pc_dispatch.c", "App/protocol/pc/pc_protocol.c",
                                   "App/protocol/pc/pc_state_machine.c", "App/protocol/pc/pc_test_message.c", "App/protocol/pc/pc_power_service.c", "HAL_Drv/ddr_power.c", "BSP/PAC1934/pac1934.c",
                                   "Middlewares/Third_Party/cJSON/cJSON.c")],
               Path(__file__).resolve().parent / "support/qemu_start.s"]
    includes = [build, *[root / p for p in ("App/protocol/pc", "App/tasks/common", "HAL_Drv",
                 "BSP/TPS62868", "BSP/PAC1934", "BSP/AD7124", "BSP/MCP4725", "BSP/Flash",
                 "Middlewares/Third_Party/cJSON", "Core/Inc")]]
    if (root / 'App/tasks/net_handoff/handoff_mcu.h').is_file():
        includes.append(root / 'App/tasks/net_handoff')
        sources.append(Path(__file__).resolve().parent / 'support/handoff_idle.c')
    command = [str(args.compiler), "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
               "-g", "-O1", "-DCJSON_NESTING_LIMIT=16", f"-DPC_TEST_EXPECTED_RSENSE={expected_rsense}",
               "-Wall", "-Wextra", "-Werror",
               "-ffunction-sections", "-fdata-sections", *["-I" + str(path) for path in includes],
               # 与正式固件使用同一精简C库及浮点输入输出，覆盖JSON实际序列化。
               *map(str, sources), "-nostartfiles", "--specs=nosys.specs", "--specs=nano.specs",
               "-Wl,-u,_printf_float", "-Wl,-u,_scanf_float",
               "-T" + str(Path(__file__).resolve().parent / "support/qemu_test.ld"), "-Wl,--gc-sections", "-o", str(build / "test.elf")]
    if (root / "b1.ioc").exists():
        command.insert(1, "-DB1_RK_UART=4")
        command.insert(1, "-I" + str(root / "App/diagnostics"))
        command.insert(1, "-I" + str(root / "App/tasks/rk"))
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
