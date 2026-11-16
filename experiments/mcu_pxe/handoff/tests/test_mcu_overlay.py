"""检查复制覆盖层与实际C交接状态机；仅离线运行，不代表真实IP交接已验收。"""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
REPO = ROOT.parents[1]
SPEC = importlib.util.spec_from_file_location('build_handoff_mcu', ROOT / 'build_handoff_mcu.py')
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)
SOURCE = Path('D:/pro/b1')
OUTPUT = REPO / 'build/mcu_pxe/handoff/tests'


class McuOverlayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.edits = BUILDER.overlay_sources(SOURCE)
        if not cls.edits:
            cls.edits = {name: (SOURCE / name).read_text(encoding='utf-8-sig') for name in (
                'Core/Src/freertos.c', 'App/tasks/rk/rk_task.c', 'App/protocol/pc/pc_test_service.c',
                'App/tasks/b2/b2_service.c', 'App/protocol/pc/pc_dispatch.c',
                'App/tasks/pc/tcp_task.c', 'LWIP/Target/ethernetif.c', 'HAL_Drv/uart_hw.c')}

    def test_unique_anchors_fail_closed(self):
        for source in ('none', 'anchor anchor'):
            with self.assertRaises(ValueError):
                BUILDER.replace(source, 'anchor', 'replacement')

    def test_never_write_formal_or_parent(self):
        for target in (SOURCE, SOURCE.parent, REPO / 'experiments/mcu_pxe/mcu',
                       REPO / 'build', REPO / 'build/mcu_pxe/b1-experiment'):
            with self.assertRaises(ValueError):
                BUILDER.prepare(SOURCE, target)

    def test_full_firmware_and_unique_uart_consumer(self):
        startup = self.edits['Core/Src/freertos.c']
        self.assertIn('xTaskCreate(StartRkTask, "rk", 8192, NULL, 3, NULL)', startup)
        self.assertNotIn('StartSerialNetTask', startup)
        rk = self.edits['App/tasks/rk/rk_task.c']
        self.assertLess(rk.index('if (handoff_mcu_rk_poll())'), rk.index('// 电源状态不是ON时'))
        self.assertIn('process_heartbeat', rk)
        service = self.edits['App/protocol/pc/pc_test_service.c']
        self.assertIn('pc_test_service_start', service)
        self.assertIn('monitor_idd_finish', service)
        self.assertNotIn('算法请求必须在任何电源/队列副作用之前拒绝', service)
        self.assertIn('handoff_mcu_busy()) return slt_status_response(sequence, "failed", "busy"', service)

    def test_entry_gates_do_not_change_busy_query(self):
        service = self.edits['App/protocol/pc/pc_test_service.c']
        original = (SOURCE / 'App/protocol/pc/pc_test_service.c').read_text(encoding='utf-8-sig')
        # busy函数原样保留，否则ARMED会因自身busy永远无法完成撤IP。
        busy_query = 'int pc_test_service_busy(void)\n{\n    return s_test.active != 0U;\n}'
        self.assertIn(busy_query, service)
        self.assertIn(busy_query, original)
        self.assertIn('s_round_active || pc_test_service_busy() || pc_power_service_busy() || handoff_mcu_busy()', service)
        self.assertIn('handoff_mcu_busy()', self.edits['App/tasks/b2/b2_service.c'])
        dispatch = self.edits['App/protocol/pc/pc_dispatch.c']
        self.assertIn('int conflict = request->command_id == PC_COMMAND_RESET_RK;', dispatch)
        self.assertIn('handoff_mcu_power_allowed(request == LOG_CONSOLE_POWER_END_RK)', dispatch)

    def test_listener_and_dma_hooks(self):
        tcp = self.edits['App/tasks/pc/tcp_task.c']
        self.assertIn('handoff_mcu_tcp_poll(1, 0)', tcp)
        self.assertIn('handoff_mcu_tcp_poll(0, listenfd >= 0)', tcp)
        self.assertIn('listenfd = handoff_open_listener()', tcp)
        eth = self.edits['LWIP/Target/ethernetif.c']
        self.assertIn('SourceAddrControl = ETH_SOURCEADDRESS_DISABLE', eth)
        self.assertIn('heth.TxDescList.BuffersInUse != 0U', eth)
        self.assertIn('DMATxDscrTab[i].DESC3 & ETH_DMATXNDESCRF_OWN', eth)
        self.assertIn('UART_FLAG_TC', self.edits['HAL_Drv/uart_hw.c'])

    def test_actual_mcu_c_contract(self):
        before = {name: hashlib.sha256((SOURCE / name).read_bytes()).hexdigest() for name in self.edits}
        snapshot = BUILDER.prepare(SOURCE, OUTPUT / 'b1-snapshot')
        after = {name: hashlib.sha256((SOURCE / name).read_bytes()).hexdigest() for name in self.edits}
        self.assertEqual(before, after)
        compiler = Path.home() / 'AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe'
        include_paths = ['Core/Inc', 'HAL_Drv', 'BSP/TPS62868', 'BSP/MCP4725', 'BSP/AD7124', 'BSP/PAC1934',
            'App/tasks/common', 'App/tasks/rk', 'App/diagnostics',
            'App/protocol/pc', 'App/protocol/rk', 'App/protocol/common',
            'Drivers/STM32H7xx_HAL_Driver/Inc', 'Drivers/CMSIS/Include',
            'Drivers/CMSIS/Device/ST/STM32H7xx/Include', 'Drivers/CMSIS/RTOS2/Include',
            'Middlewares/Third_Party/FreeRTOS/Source/include',
            'Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F',
            'Middlewares/Third_Party/LwIP/src/include', 'Middlewares/Third_Party/LwIP/system',
            'Middlewares/Third_Party/cJSON', 'LWIP/Target',
            'App/protocol/serial_net', 'App/tasks/net_handoff']
        command = [str(compiler), '-mcpu=cortex-m7', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-d16',
            '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-DSTM32H743xx', '-DUSE_HAL_DRIVER',
            '-DB1_RK_UART=1',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections,--defsym=end=_ebss']
        command += ['-I' + str(snapshot / path) for path in include_paths]
        command += [str(snapshot / 'App/protocol/serial_net/serial_net.c'), str(snapshot / 'App/protocol/serial_net/ip_guard.c'),
            str(snapshot / 'App/protocol/serial_net/net_mux.c'),
            str(snapshot / 'App/tasks/net_handoff/handoff_state.c'), str(Path(__file__).with_name('test_mcu_handoff.c')),
            str(snapshot / 'App/protocol/rk/frame.c'), str(snapshot / 'App/protocol/rk/state_machine.c'),
            str(snapshot / 'App/protocol/common/crc.c'), str(snapshot / 'App/tasks/rk/rk_link.c'),
            str(snapshot / 'App/tasks/rk/rk_task.c'),
            str(REPO / 'tests/support/qemu_start.s'), '-nostartfiles', '--specs=nosys.specs',
            '-T' + str(REPO / 'tests/support/qemu_test.ld'), '-o', str(OUTPUT / 'test.elf')]
        for mode, source in ((0, 'test_mcu_handoff.c'), (1, 'test_mcu_coexist.c')):
            with self.subTest(keep_tcp=mode):
                current = [str(Path(__file__).with_name(source)) if item ==
                           str(Path(__file__).with_name('test_mcu_handoff.c')) else item for item in command]
                current += ['-DB1_SERIAL_NET_KEEP_TCP=' + str(mode)]
                result = subprocess.run(current, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run(['D:/qemu/qemu-system-arm.exe', '-M', 'mps2-an500', '-display', 'none',
                    '-monitor', 'none', '-serial', 'none', '-no-reboot', '-semihosting-config', 'enable=on,target=native',
                    '-kernel', str(OUTPUT / 'test.elf')], capture_output=True, text=True, timeout=30)
                output = result.stdout + result.stderr
                (OUTPUT / ('result-shared.log' if mode else 'result.log')).write_text(output, encoding='utf-8')
                self.assertEqual(result.returncode, 0, output)
                self.assertIn('PASS: MCU', output)


if __name__ == '__main__':
    unittest.main(verbosity=2)
