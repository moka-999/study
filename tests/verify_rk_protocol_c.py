"""运行两板实际RK编解码和事务代码，使用QEMU替代硬件，不打开串口或烧录器。"""
import argparse
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
    # 仅生成平台桩；编解码和事务函数均直接编译目标工程源码。
    (build / 'port.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t TickType_t;
typedef void *QueueHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define configTICK_RATE_HZ 1000U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define UART_RX_BUF_SIZE 1024
#define UART_RX_GAP (-1)
#define OTA_TASK_RK 3
#define LED_HEARTBEAT 0
#define PWR_CTRL_STATE_ON 4
uint32_t xTaskGetTickCount(void);
void vTaskDelay(uint32_t);
int xQueueSend(QueueHandle_t, const void *, uint32_t);
int xQueueReceive(QueueHandle_t, void *, uint32_t);
int uart_send(int, uint8_t *, uint16_t);
int uart_poll_rx(int, uint8_t *, uint16_t);
void ota_app_progress(int);
void led_toggle(int);
int pwr_ctrl_get_state(void);
''', encoding='utf-8')
    for name in ('FreeRTOS', 'task', 'queue', 'uart_router', 'ota_app', 'led', 'pwr_ctrl'):
        (build / (name + '.h')).write_text('#include "port.h"\n', encoding='utf-8')
    # 包含目标板的任务文件以执行内部事务状态，不另写一份Python业务模型。
    (build / 'rk_task_under_test.h').write_text(
        '#include "' + (root / 'App/tasks/rk/rk_task.c').as_posix() + '"\n', encoding='utf-8')
    command = [args.compiler, '-mcpu=cortex-m7', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-d16',
               '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections']
    if root.name == 'b1' or args.b1_power_gate:
        command += ['-DB1_RK_UART=4']
    command += ['-I' + str(p) for p in (build, root / 'App/protocol/rk', root / 'App/protocol/common', root / 'App/tasks/rk')]
    command += [str(Path(__file__).with_name('test_rk_protocol.c'))]
    command += [str(root / p) for p in ('App/protocol/rk/frame.c', 'App/protocol/rk/state_machine.c',
                                       'App/protocol/common/crc.c', 'App/tasks/rk/rk_link.c')]
    boot = root.parent / 'bootloader/tests'
    command += [str(boot / 'qemu_start.s'), '-nostartfiles', '--specs=nosys.specs',
                '-T' + str(boot / 'qemu_test.ld'), '-Wl,--gc-sections', '-o', str(build / 'test.elf')]
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
