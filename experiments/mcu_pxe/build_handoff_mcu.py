"""复制完整B1并注入手动同IP实验覆盖层；仅构建，不连接硬件或启动网络服务。"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
BUILD_ROOT = ROOT.parents[1] / 'build'
HANDOFF_BUILD_ROOT = BUILD_ROOT / 'mcu_pxe/handoff'
COPY_DIRS = ('Core', 'HAL_Drv', 'BSP', 'App', 'Drivers', 'Middlewares', 'LWIP', 'boot', 'cmake', 'tools')
COPY_FILES = ('CMakeLists.txt', 'STM32H743xx_FLASH.ld', 'startup_stm32h743xx.s')


def replace(text, old, new):
    """唯一锚点不匹配就拒绝，避免源工程更新后默默漏装门禁。"""
    if text.count(old) != 1:
        raise ValueError(f'B1接入锚点应恰好一次：{old[:100]}')
    return text.replace(old, new, 1)


def with_header(text, anchor):
    return replace(text, anchor, anchor + '\n#include "handoff_mcu.h"')


def overlay_sources(src):
    """返回复制工程的修改内容；本函数从不写入src。"""
    # 已正式接入的B1直接复制；本函数的旧锚点仅供历史未接入基线使用。
    if (src / 'App/tasks/net_handoff/handoff_mcu.c').is_file():
        return {}
    edits = {}
    def read(name):
        return (src / name).read_text(encoding='utf-8-sig')

    name = 'CMakeLists.txt'
    edits[name] = read(name) + '''
# 仅实验复制工程加入同IP覆盖层，正式FUNC/PARA/SLT及原RK任务全部保留。
target_sources(b1 PRIVATE App/handoff/serial_net.c App/handoff/ip_guard.c
    App/handoff/handoff_state.c App/handoff/handoff_mcu.c)
target_include_directories(b1 PRIVATE App/handoff)
target_compile_definitions(b1 PRIVATE ENABLE_MANUAL_IP_HANDOFF=1)
'''
    name = 'Core/Src/freertos.c'
    text = with_header(read(name), '#include <stdio.h>')
    text = replace(text, 'if (!b2_service_prepare()) Error_Handler();',
                   'if (!b2_service_prepare() || !handoff_mcu_prepare()) Error_Handler();')
    text = replace(text, 'log_console_start();', '''log_console_start();
    static const char handoff_banner[] = "[handoff] H1B DHCP/baud; full B1; OFF: net rk, then 050x boot\\r\\n";
    (void)log_console_write(handoff_banner, sizeof(handoff_banner) - 1U);''')
    # RK任务不能被实验任务替换，启动锚点也作为版本漂移检查。
    if text.count('xTaskCreate(StartRkTask, "rk", 8192, NULL, 3, NULL)') != 1:
        raise ValueError('完整B1的唯一RK任务锚点不匹配')
    edits[name] = text

    name = 'HAL_Drv/uart_hw.h'
    edits[name] = replace(read(name), '#endif /* UART_HW_H */',
        '/* 1表示TX环与硬件移位寄存器均空；不改变TC标记。 */\nint uart_hw_tx_idle(uart_if_t *port);\n'
        '/* 任务侧且TX空闲时改速率；停止RX，调用者负责重新启动。返回IF_OK/IF_ERR。 */\n'
        'int uart_hw_set_baud(uart_if_t *port, uint32_t baud);\n\n#endif /* UART_HW_H */')
    name = 'HAL_Drv/uart_hw.c'
    edits[name] = read(name) + '''
/* 交接前证明全部UART发送已完成，不能用最后一帧入队时间代替TC。 */
int uart_hw_tx_idle(uart_if_t *port)
{
    UART_HandleTypeDef *uart = port ? port->handle : NULL;
    uart_tx_ring_t *ring = uart ? find_tx_ring(uart) : NULL;
    if (!ring || !ring->mutex) return 0;
    uint32_t saved = __get_PRIMASK();
    __disable_irq();
    int idle = ring->head == ring->tail && __HAL_UART_GET_FLAG(uart, UART_FLAG_TC) != RESET;
    __set_PRIMASK(saved);
    return idle;
}

/* 不阻塞等TX，也不在临界区调用HAL；保留协议端口的自定义TX中断钩子。 */
int uart_hw_set_baud(uart_if_t *port, uint32_t baud)
{
    UART_HandleTypeDef *uart = port ? port->handle : NULL;
    uart_tx_ring_t *ring = uart ? find_tx_ring(uart) : NULL;
    if (!baud || !ring || !ring->mutex || xSemaphoreTake(ring->mutex, 0U) != pdTRUE)
        return IF_ERR;
    int result = IF_ERR;
    uint32_t previous = uart->Init.BaudRate;
    if (uart_hw_tx_idle(port) && uart_rx_abort(port) == IF_OK) {
        uart->Init.BaudRate = baud;
        if (HAL_UART_Init(uart) == HAL_OK) result = IF_OK;
        else {
            uart->Init.BaudRate = previous;
            (void)HAL_UART_Init(uart);
        }
        uart->TxISR = uart_tx_drain;
    }
    xSemaphoreGive(ring->mutex);
    return result;
}
'''
    name = 'HAL_Drv/uart_router.h'
    edits[name] = replace(read(name), '#endif /* UART_ROUTER_H */',
        '/* 只读发送空闲证明：TX环空且最后停止位已发出。 */\nint uart_tx_idle(int id);\n'
        '/* 唯一RK消费任务在已协商的静默窗口调用；清旧RX，返回IF_OK/IF_ERR。 */\n'
        'int uart_set_baud(int id, uint32_t baud);\n\n#endif /* UART_ROUTER_H */')
    name = 'HAL_Drv/uart_router.c'
    edits[name] = read(name) + '''
int uart_tx_idle(int id)
{
    return slot_is_registered(id) && uart_hw_tx_idle(s_slots[id].interface);
}

int uart_set_baud(int id, uint32_t baud)
{
    if (id != UART_PORT_RK_PROTOCOL || !slot_is_registered(id) || !uart_tx_idle(id))
        return IF_ERR;
    uart_slot_t *slot = &s_slots[id];
    taskENTER_CRITICAL();
    slot->recovery_pending = 1U; /* 阻止RX回调在改速期间重启DMA。 */
    taskEXIT_CRITICAL();
    int result = uart_hw_set_baud(slot->interface, baud);
    taskENTER_CRITICAL();
    xQueueReset(slot->rx_queue);
    slot->gap_pending = 0U;
    slot->recovery_pending = 0U;
    start_receive(id);
    if (slot->recovery_pending) result = IF_ERR;
    taskEXIT_CRITICAL();
    return result;
}
'''
    name = 'App/tasks/rk/rk_task.c'
    text = with_header(read(name), '#include "uart_router.h"')
    text = replace(text, '#include "handoff_mcu.h"', '#include "handoff_mcu.h"\n#include "handoff_rk.h"')
    text = replace(text, 'void StartRkTask(void *argument)', '''/* H1复用正式SPL处理；上电过渡仅接收频率查询，OFF不应答残留帧。 */
void rk_task_handoff_receive(const tlv_frame_t *frame)
{
    if (pwr_ctrl_get_state() == PWR_CTRL_STATE_ON || startup_rx_allowed()) accept_frame(frame);
}

void rk_task_handoff_heartbeat(void)
{
    static TickType_t last_tick;
    process_heartbeat(&last_tick);
}

/* TCP只读门禁；不取队列、不推进原RK业务，唯一消费者仍是StartRkTask。 */
int rk_task_handoff_idle(void)
{
    taskENTER_CRITICAL();
    int idle = !s_pending.waiting && !s_pending.delivery && !s_pending.recovered &&
        rkRequestQueue && rkResponseQueue && uxQueueMessagesWaiting(rkRequestQueue) == 0U &&
        uxQueueMessagesWaiting(rkResponseQueue) == 0U;
    taskEXIT_CRITICAL();
    return idle;
}

void StartRkTask(void *argument)''')
    text = replace(text, '    //循环读取\n    for (;;) {', '''    //循环读取
    for (;;) {
        /* 手动模式先识别SPL/发003E，进入EDK后停TLV，始终由本任务消费UART。 */
        if (handoff_mcu_rk_poll()) {
            tlv_sm_reset(&stream);
            last_heartbeat = xTaskGetTickCount();
            task_watchdog_progress(TASK_WATCHDOG_RK);
            vTaskDelay(pdMS_TO_TICKS(1U));
            continue;
        }''')
    edits[name] = text

    name = 'LWIP/Target/ethernetif.c'
    text = with_header(read(name), '#include "board_identity.h"')
    text = replace(text, 'hal_eth_init_status = HAL_ETH_Init(&heth);', '''hal_eth_init_status = HAL_ETH_Init(&heth);
  /* 网桥必须保留RK源MAC；HAL默认替换为MCU MAC会破坏同IP授权及DHCP绑定。 */
  ETH_MACConfigTypeDef handoff_mac;
  if (hal_eth_init_status == HAL_OK && HAL_ETH_GetMACConfig(&heth, &handoff_mac) == HAL_OK) {
    handoff_mac.SourceAddrControl = ETH_SOURCEADDRESS_DISABLE;
    if (HAL_ETH_SetMACConfig(&heth, &handoff_mac) != HAL_OK) Error_Handler();
  } else Error_Handler();''')
    text = replace(text, 'netif->mtu = ETH_MAX_PAYLOAD;',
                   'netif->mtu = ETH_MAX_PAYLOAD;\n  handoff_mcu_attach(netif);')
    text = replace(text, 'if (netif->input( p, netif) != ERR_OK )',
                   'if (handoff_mcu_input(p) || netif->input(p, netif) != ERR_OK)')
    text += '''
/* 仅tcpip线程调用；先回收完成引用，再逐描述符确认DMA已不持有TX。 */
int handoff_eth_tx_idle(void)
{
  HAL_ETH_ReleaseTxPacket(&heth);
  if (heth.TxDescList.BuffersInUse != 0U) return 0;
  for (unsigned i = 0U; i < ETH_TX_DESC_CNT; ++i)
    if (DMATxDscrTab[i].DESC3 & ETH_DMATXNDESCRF_OWN) return 0;
  return 1;
}
'''
    edits[name] = text

    name = 'App/tasks/pc/tcp_task.c'
    text = with_header(read(name), '#include "b2_service.h"')
    begin = text.index('    /* 阶段2：建立整个任务生命周期内唯一的监听socket。 */')
    end = text.index('    /*\n     * 阶段3：单客户端accept循环。', begin)
    old = text[begin:end]
    helper = old.replace('    /* 阶段2：建立整个任务生命周期内唯一的监听socket。 */\n', '')
    helper = helper.replace('        vTaskDelete(NULL);\n        return;', '        return -1;')
    helper += '    return listenfd;\n'
    text = replace(text, old, '    int listenfd = -1;\n\n')
    text = replace(text, 'void StartTcpServerTask(void *argument)',
        '/* listen的创建/关闭始终由唯一TCP任务负责。 */\nstatic int handoff_open_listener(void)\n{\n' + helper + '}\n\nvoid StartTcpServerTask(void *argument)')
    text = replace(text, '    for (;;) {\n        TCP_DIAG(3, 0xBEEF0004);', '''    for (;;) {
        pc_dispatch_poll();
        handoff_mcu_tcp_poll(0, listenfd >= 0);
        if (!handoff_mcu_listen_allowed() && listenfd >= 0) {
            close(listenfd);
            listenfd = -1;
        }
        if (handoff_mcu_listen_allowed() && listenfd < 0) {
            listenfd = handoff_open_listener();
            if (listenfd >= 0) handoff_mcu_tcp_poll(0, 1);
        }
        if (listenfd < 0) {
            b2_service_poll(0);
            task_watchdog_progress(TASK_WATCHDOG_TCP);
            vTaskDelay(pdMS_TO_TICKS(20U));
            continue;
        }
        TCP_DIAG(3, 0xBEEF0004);''')
    text = replace(text, '            b2_service_poll(1);',
                   '            b2_service_poll(1);\n            handoff_mcu_tcp_poll(1, 1);')
    edits[name] = text

    name = 'App/tasks/b2/b2_service.c'
    text = with_header(read(name), '#include "network_config.h"')
    text = replace(text, 'if (connected || pc_test_service_busy() || pc_power_service_busy()) return 1;',
        'if (connected || pc_test_service_busy() || pc_power_service_busy() || handoff_mcu_busy()) return 1;')
    edits[name] = text

    name = 'App/protocol/pc/pc_test_service.c'
    text = with_header(read(name), '#include "pc_test_service.h"')
    text = replace(text, 'if (!kind || !start || !response) return PC_DISPATCH_ERROR;', '''if (!kind || !start || !response) return PC_DISPATCH_ERROR;
    if (handoff_mcu_busy())
        return status_response(request_command, sequence, start->algorithm_index, "busy", response);''')
    text = replace(text, 'if (!start || !response) return PC_DISPATCH_ERROR;', '''if (!start || !response) return PC_DISPATCH_ERROR;
    if (handoff_mcu_busy()) return slt_status_response(sequence, "failed", "busy", response);''')
    text = replace(text, 'if (s_round_active || pc_test_service_busy() || pc_power_service_busy())',
                   'if (s_round_active || pc_test_service_busy() || pc_power_service_busy() || handoff_mcu_busy())')
    edits[name] = text

    name = 'App/protocol/pc/pc_dispatch.c'
    text = with_header(read(name), '#include "pc_dispatch.h"')
    text = replace(text, '    /* 小表顺序查找，命中只执行一次；未命中返回notSupported，不隐式转发RK。 */', '''    /* 手动交接期间保留查询；ARMED可0501/Boot/电压启动，0505仍拒绝以免等SPL心跳。 */
    if (handoff_mcu_busy()) {
        int conflict = request->command_id == PC_COMMAND_RESET_RK;
        for (size_t i = 0U; i < sizeof(test_conflicts) / sizeof(test_conflicts[0]); ++i)
            if (request->command_id == test_conflicts[i] &&
                !handoff_mcu_power_allowed(request->command_id == PC_COMMAND_POWER_OFF)) conflict = 1;
        if (conflict) {
            cJSON_Delete(root);
            return build_command_failure(request, mcu_tx_sequence, "busy", response);
        }
    }

    /* 小表顺序查找，命中只执行一次；未命中返回notSupported，不隐式转发RK。 */''')
    text = replace(text, '    if (request == LOG_CONSOLE_POWER_NONE) return;', '''    if (request == LOG_CONSOLE_POWER_NONE) return;
    if (!handoff_mcu_power_allowed(request == LOG_CONSOLE_POWER_END_RK)) {
        log_console_complete_power_request("busy");
        return;
    }''')
    edits[name] = text

    name = 'Core/Src/log_console.c'
    text = with_header(read(name), '#include "log_console.h"')
    text = replace(text, '    } else if (!s_discard_line && strncmp(s_command, "led ", 4U) == 0) {', '''    } else if (!s_discard_line &&
               (strcmp(s_command, "net rk") == 0 || strcmp(s_command, "net mcu") == 0)) {
        handoff_command_t request = strcmp(s_command, "net rk") == 0 ? HANDOFF_COMMAND_RK : HANDOFF_COMMAND_MCU;
        s_reply = handoff_mcu_console_request(request) ?
            "\\r\\n[handoff] command queued\\r\\n" : "\\r\\n[handoff] command busy\\r\\n";
    } else if (!s_discard_line && strncmp(s_command, "led ", 4U) == 0) {''')
    text = replace(text, 'status | start rk | end rk | led', 'status | start rk | end rk | net rk | net mcu | led')
    edits[name] = text
    return edits


def prepare(src, dest):
    src, dest = src.resolve(), dest.resolve()
    allowed = HANDOFF_BUILD_ROOT.resolve()
    if not dest.is_relative_to(allowed) or dest == allowed or dest == src or src.is_relative_to(dest):
        raise ValueError('实验输出必须是build/mcu_pxe/handoff内的独立子目录，不覆盖旧D4')
    edits = overlay_sources(src)
    # 先核对全部锚点，再复制；不删除目录，不触及正式源码及旧D4实验目录。
    dest.mkdir(parents=True, exist_ok=True)
    for name in COPY_DIRS:
        shutil.copytree(src / name, dest / name, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns('build', '__pycache__', '.git'))
    for name in COPY_FILES:
        shutil.copy2(src / name, dest / name)
    if edits:
        module_dir = dest / 'App/handoff'
        module_dir.mkdir(parents=True, exist_ok=True)
        for folder in ('common', 'mcu'):
            for path in (ROOT / 'handoff' / folder).glob('*.[ch]'):
                shutil.copy2(path, module_dir / path.name)
    patch = []
    for name, text in edits.items():
        original = (src / name).read_text(encoding='utf-8-sig')
        patch.extend(difflib.unified_diff(original.splitlines(True), text.splitlines(True),
                                       fromfile='a/' + name, tofile='b/' + name))
        (dest / name).write_text(text, encoding='utf-8', newline='\n')
    (dest.parent / 'handoff-integration.patch').write_text(''.join(patch), encoding='utf-8')
    manifest = {str(path.relative_to(dest)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in dest.rglob('*') if path.is_file() and 'build' not in path.relative_to(dest).parts}
    (dest.parent / 'handoff-source-sha256.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    return dest


def build(dest):
    bundle = Path.home() / 'AppData/Local/stm32cube/bundles'
    cmake = bundle / 'cmake/4.3.1+st.1/bin/cmake.exe'
    ninja = bundle / 'ninja/1.13.2+st.1/bin/ninja.exe'
    output = dest / 'build/Debug'
    commands = [[str(cmake), '-S', str(dest), '-B', str(output), '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Debug', '-DCMAKE_TOOLCHAIN_FILE=' + str(dest / 'cmake/gcc-arm-none-eabi.cmake'),
        '-DCMAKE_MAKE_PROGRAM=' + str(ninja), '-DPython3_EXECUTABLE=' + sys.executable],
        [str(cmake), '--build', str(output), '-j', '8']]
    with (dest.parent / 'handoff-mcu-build.log').open('w', encoding='utf-8') as log:
        for command in commands:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=300)
            if result.returncode:
                raise RuntimeError('构建失败：' + str(dest.parent / 'handoff-mcu-build.log'))
    artifacts = dest.parent / 'artifacts'
    artifacts.mkdir(exist_ok=True)
    for suffix in ('elf', 'bin', 'manifest.json'):
        shutil.copy2(output / ('b1.' + suffix), artifacts / ('b1_handoff.' + suffix))
    return artifacts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--b1', type=Path, default=Path('D:/pro/b1'))
    parser.add_argument('--out', type=Path, default=HANDOFF_BUILD_ROOT / 'b1-snapshot')
    parser.add_argument('--prepare-only', action='store_true')
    args = parser.parse_args()
    dest = prepare(args.b1, args.out)
    print('完整B1实验复制工程：', dest)
    if not args.prepare_only:
        print('构建通过，未烧录：', build(dest))


if __name__ == '__main__':
    main()
