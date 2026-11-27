"""用QEMU运行实际TCP任务及帧状态机；不访问网卡、串口或烧录器。"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--compiler', default='C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe')
    parser.add_argument('--qemu', default='D:/qemu/qemu-system-arm.exe')
    args = parser.parse_args()
    root = args.app_root.resolve()
    build = root / 'build/tcp-single-client-test'
    build.mkdir(parents=True, exist_ok=True)
    (build / 'lwip').mkdir(exist_ok=True)
    # 仅替换RTOS、socket及硬件边界；任务循环和PC帧解析来自待测工程。
    (build / 'port.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t TickType_t;
typedef unsigned socklen_t;
struct timeval { long tv_sec, tv_usec; };
struct in_addr { uint32_t s_addr; };
struct sockaddr { uint8_t sa_len, sa_family; char sa_data[14]; };
struct sockaddr_in { uint8_t sin_len, sin_family; uint16_t sin_port; struct in_addr sin_addr; };
typedef struct { unsigned bits; } fd_set;
struct netif { struct { uint32_t addr; } ip_addr; };
#define FD_ZERO(s) ((s)->bits=0U)
#define FD_SET(n,s) ((s)->bits|=1U<<(n))
#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_RCVTIMEO 3
#define SO_SNDTIMEO 4
#define INADDR_ANY 0U
#define B1_PC_TCP_PORT 8888U
#define TASK_WATCHDOG_TCP 1
#define LED_CONNECT 0
#define EWOULDBLOCK 11
#define EAGAIN 11
#define ECONNRESET 104
#define EBADF 9
#define ENOMEM 12
#define errno test_errno
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
extern int test_errno;
uint32_t xTaskGetTickCount(void);
void vTaskDelay(uint32_t);
void vTaskDelete(void *);
void task_watchdog_progress(int);
void led_on(int);
void led_off(int);
void b2_service_poll(int);
void handoff_mcu_tcp_poll(int,int);
int handoff_mcu_listen_allowed(void);
uint16_t htons(uint16_t);
uint16_t ntohs(uint16_t);
int socket(int,int,int);
int setsockopt(int,int,int,const void*,socklen_t);
int bind(int,const struct sockaddr*,socklen_t);
int listen(int,int);
int select(int,fd_set*,fd_set*,fd_set*,struct timeval*);
int accept(int,struct sockaddr*,socklen_t*);
int close(int);
int recv(int,void*,size_t,int);
int send(int,const void*,size_t,int);
''', encoding='utf-8')
    for name in ('main.h', 'FreeRTOS.h', 'task.h', 'app_tasks.h', 'task_watchdog.h',
                 'b2_service.h', 'handoff_mcu.h', 'board_network_config.h', 'led.h',
                 'lwip/sockets.h', 'lwip/netif.h', 'lwip/errno.h'):
        (build / name).write_text('#include "port.h"\n', encoding='utf-8')
    b1_trace = 'pc_trace.h' in (root / 'App/tasks/pc/tcp_task.c').read_text(encoding='utf-8')
    (build / 'task_under_test.h').write_text(
        '#include "' + (root / 'App/tasks/pc/tcp_task.c').as_posix() + '"\n', encoding='utf-8')
    tests = Path(__file__).resolve().parent
    support = tests / 'support'
    command = [args.compiler, '-mcpu=cortex-m7', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-d16',
               '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections',
               '-I' + str(build), '-I' + str(root / 'App/protocol/pc')]
    if b1_trace:
        command += ['-DTEST_B1_TRACE=1', '-I' + str(root / 'App/diagnostics')]
    command += [str(tests / 'test_tcp_single_client.c'), str(root / 'App/protocol/pc/pc_state_machine.c'),
                str(support / 'qemu_start.s'), '-nostartfiles', '--specs=nosys.specs',
                '-T' + str(support / 'qemu_test.ld'), '-Wl,--gc-sections,--defsym=end=_ebss',
                '-o', str(build / 'test.elf')]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (build / 'build.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run([args.qemu, '-M', 'mps2-an500', '-display', 'none', '-monitor', 'none',
                             '-serial', 'none', '-no-reboot', '-semihosting-config', 'enable=on,target=native',
                             '-kernel', str(build / 'test.elf')], capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / 'run.log').write_text(output, encoding='utf-8')
    if result.returncode or 'PASS: TCP single-client task' not in output:
        raise RuntimeError(output)
    print(root.name + ': ' + output.strip())


if __name__ == '__main__':
    main()
