"""运行两板实际RK编解码和事务代码，使用QEMU替代硬件，不打开串口或烧录器。"""
import argparse
import re
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--b1-power-gate', action='store_true',
                        help='使用当前源码离线核验B1掉电门禁，不修改独立B1工程')
    parser.add_argument('--compiler', default='C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe')
    parser.add_argument('--qemu', default='D:/qemu/qemu-system-arm.exe')
    args = parser.parse_args()
    root = args.app_root.resolve()
    build = root / ('build/rk-protocol-test-b1-gate' if args.b1_power_gate else 'build/rk-protocol-test')
    build.mkdir(parents=True, exist_ok=True)
    # 枚举从目标板正式头读取，避免测试桩把V2电源阶段错误套到B1。
    state_enum = re.search(r'typedef enum\s*\{\s*PWR_CTRL_STATE_OFF\b.*?\}\s*pwr_ctrl_state_t;',
                          (root / 'HAL_Drv/pwr_ctrl.h').read_text(encoding='utf-8'), re.S).group(0)
    # 仅生成平台桩；编解码和事务函数均直接编译目标工程源码。
    (build / 'port.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t TickType_t;
typedef void *QueueHandle_t;
typedef struct { int locked; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef struct { uint32_t unused; } GPIO_TypeDef;
#define portMAX_DELAY UINT32_MAX
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *);
int xSemaphoreTake(SemaphoreHandle_t, uint32_t);
int xSemaphoreGive(SemaphoreHandle_t);
#define UART_PORT_RK_PROTOCOL 0
int uart_send_tracked(int, uint8_t *, uint16_t, uint32_t *);
int uart_tx_status(int, uint32_t, uint32_t *);
#define pdTRUE 1
#define pdFALSE 0
#define configTICK_RATE_HZ 1000U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define UART_RX_BUF_SIZE 1024
#define UART_RX_GAP (-1)
#define TASK_WATCHDOG_RK 3
#define LED_HEARTBEAT 0
/* PWR_STATE_ENUM */
uint32_t xTaskGetTickCount(void);
void vTaskDelay(uint32_t);
int xQueueSend(QueueHandle_t, const void *, uint32_t);
int xQueueReceive(QueueHandle_t, void *, uint32_t);
int uart_send(int, uint8_t *, uint16_t);
int uart_poll_rx(int, uint8_t *, uint16_t);
void task_watchdog_progress(int);
void led_toggle(int);
pwr_ctrl_state_t pwr_ctrl_get_state(void);
'''.replace('/* PWR_STATE_ENUM */', state_enum), encoding='utf-8')
    for name in ('FreeRTOS', 'task', 'queue', 'uart_router', 'task_watchdog', 'led', 'pwr_ctrl', 'semphr'):
        (build / (name + '.h')).write_text('#include "port.h"\n', encoding='utf-8')
    (build / 'main.h').write_text('#include "port.h"\n#define HAL_GetTick xTaskGetTickCount\n', encoding='utf-8')
    (build / 'monitor_task.h').write_text('''#include "memory_monitor.h"
typedef void (*monitor_sample_observer_t)(const memory_monitor_sample_t *, uint8_t, uint32_t, uint32_t);
void monitor_set_sample_observer(monitor_sample_observer_t);
''', encoding='utf-8')
    # 包含目标板的任务文件以执行内部事务状态，不另写一份Python业务模型。
    (build / 'rk_task_under_test.h').write_text(
        '#include "' + (root / 'App/tasks/rk/rk_task.c').as_posix() + '"\n', encoding='utf-8')
    command = [args.compiler, '-mcpu=cortex-m7', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-d16',
               '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections']
    if root.name == 'b1' or args.b1_power_gate:
        command += ['-DB1_RK_UART=4']
    command += ['-I' + str(p) for p in (build, root / 'App/protocol/rk', root / 'App/protocol/common', root / 'App/tasks/rk', root / 'HAL_Drv', root / 'BSP/AD7124', root / 'BSP/PAC1934')]
    command += [str(Path(__file__).with_name('test_rk_protocol.c'))]
    command += [str(root / p) for p in ('App/protocol/rk/frame.c', 'App/protocol/rk/state_machine.c',
                                       'App/protocol/common/crc.c', 'App/tasks/rk/rk_link.c', 'App/tasks/rk/rk_idd.c',
                                       'App/protocol/rk/idd_session.c', 'HAL_Drv/idd_stats.c')]
    support = Path(__file__).resolve().parent / 'support'
    if root.name == 'b1':
        (build / 'main.h').write_text('#include "port.h"\n#define HAL_GetTick xTaskGetTickCount\n', encoding='utf-8')
        command += ['-DTEST_B1_PROTOCOL_TRACE=1', '-I' + str(support), '-I' + str(root / 'App/diagnostics'),
                    '-I' + str(root / 'Core/Inc'), str(support / 'trace_capture.c'),
                    str(root / 'App/diagnostics/protocol_trace.c'), str(root / 'App/diagnostics/rk_trace.c'),
                    '-Wl,--defsym=end=_ebss']
    command += [str(support / 'qemu_start.s'), '-nostartfiles', '--specs=nosys.specs',
                '-T' + str(support / 'qemu_test.ld'), '-Wl,--gc-sections,--defsym=end=_ebss', '-o', str(build / 'test.elf')]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (build / 'build.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run([args.qemu, '-M', 'mps2-an500', '-display', 'none', '-monitor', 'none',
                             '-serial', 'none', '-no-reboot', '-semihosting-config', 'enable=on,target=native',
                             '-kernel', str(build / 'test.elf')], capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / 'run.log').write_text(output, encoding='utf-8')
    if result.returncode or 'PASS: RK protocol and transaction' not in output:
        raise RuntimeError(output)
    print(root.name + ': ' + output.strip())


if __name__ == '__main__':
    main()
