"""实际B1网络控制台与lwIP TCP/IP离线交互；不连接或烧录板卡。"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app-root', type=Path, default=Path('D:/pro/b1'))
    parser.add_argument('--compiler', default='C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe')
    parser.add_argument('--qemu', default='D:/qemu/qemu-system-arm.exe')
    args = parser.parse_args()
    root = args.app_root.resolve()
    tests = Path(__file__).resolve().parent
    build = root / 'build/net-console-test'
    build.mkdir(parents=True, exist_ok=True)
    for folder in ('arch', 'lwip'):
        (build / folder).mkdir(exist_ok=True)
    (build / 'arch/cc.h').write_text('''#pragma once
#include <stdint.h>
void test_assert(const char *);
#define LWIP_PLATFORM_ASSERT(s) test_assert(s)
#define LWIP_PLATFORM_DIAG(x) ((void)0)
''', encoding='utf-8')
    # 使用正式网络内存参数；仅将调度替换为可控回调，TCP/IP及内存池保留真实代码。
    (build / 'lwipopts.h').write_text('''#pragma once
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_TIMERS 0
#define LWIP_DHCP 0
#define LWIP_AUTOIP 0
#define LWIP_ARP 0
#define LWIP_ETHERNET 0
#define LWIP_IPV6 0
#define MEM_SIZE (30*1024)
#define MEM_ALIGNMENT 4
#define MEMP_NUM_TCP_PCB 5
#define TCP_SND_BUF (12*1024)
#define TCP_SND_QUEUELEN 9
#define LWIP_DISABLE_TCP_SANITY_CHECKS 1
#define MEMP_NUM_TCP_SEG 36
#define PBUF_POOL_SIZE 32
#define SO_REUSE 1
#define LWIP_STATS 1
#define TCP_LISTEN_BACKLOG 0
''', encoding='utf-8')
    (build / 'lwip/tcpip.h').write_text('''#pragma once
#include "lwip/err.h"
err_t tcpip_try_callback(void (*)(void *), void *);
''', encoding='utf-8')
    for name in ('FreeRTOS.h', 'task.h'):
        (build / name).write_text('''#pragma once
void test_enter(void);
void test_exit(void);
#define taskENTER_CRITICAL() test_enter()
#define taskEXIT_CRITICAL() test_exit()
''', encoding='utf-8')
    lwip = root / 'Middlewares/Third_Party/LwIP/src'
    sources = ['core/init.c', 'core/def.c', 'core/inet_chksum.c', 'core/ip.c', 'core/mem.c',
               'core/memp.c', 'core/netif.c', 'core/pbuf.c', 'core/stats.c', 'core/tcp.c', 'core/timeouts.c',
               'core/tcp_in.c', 'core/tcp_out.c', 'core/udp.c', 'core/raw.c',
               'core/ipv4/ip4.c', 'core/ipv4/ip4_addr.c', 'core/ipv4/ip4_frag.c', 'core/ipv4/icmp.c']
    command = [args.compiler, '-mcpu=cortex-m7', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-d16',
               '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections',
               *['-I' + str(p) for p in (build, lwip / 'include', root / 'Core/Inc', root / 'App/tasks/console')],
               str(tests / 'test_net_console.c'), str(root / 'App/tasks/console/net_console.c'),
               *[str(lwip / p) for p in sources], str(tests / 'support/qemu_start.s'),
               '-nostartfiles', '--specs=nosys.specs', '-T' + str(tests / 'support/qemu_test.ld'),
               '-Wl,--gc-sections,--defsym=end=_ebss', '-o', str(build / 'test.elf')]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (build / 'build.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run([args.qemu, '-M', 'mps2-an500', '-display', 'none', '-monitor', 'none',
                             '-serial', 'none', '-no-reboot', '-semihosting-config', 'enable=on,target=native',
                             '-kernel', str(build / 'test.elf')], capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / 'run.log').write_text(output, encoding='utf-8')
    if result.returncode or 'PASS: actual lwIP network console' not in output:
        raise RuntimeError(output)
    print(output.strip())


if __name__ == '__main__':
    main()
