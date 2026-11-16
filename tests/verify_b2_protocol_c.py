"""在QEMU执行实际B2协议/任务/业务和SPI错误分类；不连接外设、不模拟DMA时序。"""
import argparse
from pathlib import Path
import subprocess


def run_suite(args, command, build, label, marker):
    """保存编译和运行输出；必须正常退出且出现通过标志，才判定本组成功。"""
    build.mkdir(parents=True, exist_ok=True)
    elf = build / "test.elf"
    result = subprocess.run([*command, "-o", str(elf)], capture_output=True, text=True, timeout=60)
    (build / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run([args.qemu, "-M", "mps2-an500", "-display", "none", "-monitor", "none",
                             "-serial", "none", "-no-reboot", "-semihosting-config", "enable=on,target=native",
                             "-kernel", str(elf)], capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / "run.log").write_text(output, encoding="utf-8")
    if result.returncode or marker not in output:
        raise RuntimeError(output)
    print(label + ": " + output.strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--compiler", default="C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe")
    parser.add_argument("--qemu", default="D:/qemu/qemu-system-arm.exe")
    args = parser.parse_args()
    root = args.app_root.resolve()
    test_root = Path(__file__).resolve().parents[1]
    build = root / "build/b2-protocol-test"
    build.mkdir(parents=True, exist_ok=True)
    # 只替代外设/调度端口，待测C逻辑直接包含目标工程源码，不另写协议模型。
    port = r'''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef void *QueueHandle_t;
typedef uint32_t TickType_t;
#define pdPASS 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
void test_assert(int);
#define configASSERT(x) test_assert(!!(x))
QueueHandle_t xQueueCreate(unsigned, unsigned);
int xQueueSend(QueueHandle_t, const void *, uint32_t);
int xQueueReceive(QueueHandle_t, void *, uint32_t);
void vTaskDelay(uint32_t);
void vTaskDelete(void *);
uint32_t HAL_GetTick(void);
int pc_test_service_busy(void);
int pc_power_service_busy(void);
int pc_test_round_update_modes(uint8_t, const uint8_t *);
'''
    (build / "port.h").write_text(port, encoding="utf-8")
    for name in ("FreeRTOS", "queue", "task", "main", "pc_test_service", "pc_power_service"):
        (build / (name + ".h")).write_text('#include "port.h"\n', encoding="utf-8")
    (build / "b2_under_test.h").write_text(
        '#include "' + (root / "App/tasks/b2/b2_service.c").as_posix() + '"\n'
        '#include "' + (root / "App/tasks/b2/b2_task.c").as_posix() + '"\n', encoding="utf-8")
    includes = [build, root / "App/protocol/b2", root / "App/protocol/common",
                root / "App/tasks/b2", root / "HAL_Drv", test_root / "tools/b2_spi_master"]
    sources = [test_root / "tests/test_b2_protocol.c", test_root / "tools/b2_spi_master/wire.c"]
    sources += [root / p for p in ("App/protocol/b2/b2_protocol.c",
                                   "App/protocol/b2/b2_state_machine.c", "App/protocol/common/crc.c")]
    support = Path(__file__).resolve().parent / "support"
    if (root / 'App/tasks/net_handoff/handoff_mcu.h').is_file():
        includes.append(root / 'App/tasks/net_handoff')
        sources.append(support / 'handoff_idle.c')
    flags = [args.compiler, "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
               "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-ffunction-sections", "-fdata-sections"]
    link_flags = [str(support / "qemu_start.s"), "-nostartfiles", "--specs=nosys.specs",
                  "-T" + str(support / "qemu_test.ld"), "-Wl,--gc-sections"]
    command = flags + ["-I" + str(p) for p in includes] + list(map(str, sources)) + link_flags
    run_suite(args, command, build, root.name, "PASS: B2 protocol")

    # 使用目标板真实HAL/CMSIS/RTOS头，不引入上述队列桩；只调用纯寄存器值分类函数。
    driver_includes = [root / p for p in (
        "HAL_Drv", "App/protocol/b2", "Core/Inc", "Drivers/STM32H7xx_HAL_Driver/Inc",
        "Drivers/CMSIS/Include", "Drivers/CMSIS/Device/ST/STM32H7xx/Include",
        "Middlewares/Third_Party/FreeRTOS/Source/include",
        "Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F")]
    command = flags + ["-DSTM32H743xx", "-DUSE_HAL_DRIVER"]
    if root.name == "b1":
        command += ["-DB1_RK_UART=4"]
    command += ["-I" + str(p) for p in driver_includes]
    command += [str(test_root / "tests/test_b2_spi_errors.c"), *link_flags]
    run_suite(args, command, build / "spi-errors", root.name, "PASS: actual SPI DMA error classifier")
    print("实际C协议与错误分类通过；本轮未验证SPI/DMA时序、电气和真实netif。")


if __name__ == "__main__":
    main()
