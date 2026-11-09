#!/usr/bin/env python3
"""接口边界与UART唤醒策略的离线检查；不是C运行测试或真板并发验收。"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def code(relative: str) -> str:
    """去掉注释，避免旧名出现在解释文字里影响结构检查。"""
    source = (ROOT / relative).read_text(encoding="utf-8")
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)


def main() -> None:
    passed = 0
    hal = code("HAL_Drv/hal_if.h")
    assert "UART_HandleTypeDef" not in hal and "uart_tx_" not in hal
    assert '"main.h"' not in hal and '"FreeRTOS.h"' not in hal
    passed += 1

    pc_header = code("App/protocol/pc/pc_protocol.h")
    pc_source = code("App/protocol/pc/pc_protocol.c")
    assert '"cJSON.h"' not in pc_header
    assert "typedef struct cJSON cJSON;" in pc_header
    assert '#include "cJSON.h"' in pc_source
    passed += 1

    # 纯PC组帧状态机不依赖具体命令、socket、Flash或RK。
    parser = code("App/protocol/pc/pc_state_machine.c")
    for name in ("cJSON_", "PC_COMMAND_", "flash_if", "lwip/", "rk_link"):
        assert name not in parser, name
    passed += 1

    instances = code("HAL_Drv/board_interfaces.h")
    expected = {"i2c1_if", "i2c2_if", "i2c3_if", "i2c4_if", "spi2_if", "uart1_if", "uart2_if"}
    found = re.findall(r"extern\s+(?:i2c|spi|uart)_if_t\s+(\w+)\s*;", instances)
    assert set(found) == expected and len(found) == len(expected)
    for base in ("App", "HAL_Drv"):
        for path in (ROOT / base).rglob("*.c"):
            source = code(str(path.relative_to(ROOT)))
            assert not re.search(r"extern\s+(?:i2c|spi|uart)_if_t", source), path
    passed += 1

    link = code("App/tasks/rk/rk_link.c")
    link_header = code("App/tasks/rk/rk_link.h")
    assert "static uint64_t s_next_sequence;" in link
    assert link.index("taskENTER_CRITICAL();") < link.index("s_next_sequence++")
    assert link.index("s_next_sequence++") < link.index("taskEXIT_CRITICAL();")
    assert "extern QueueHandle_t rkRequestQueue;" in link_header
    assert "extern QueueHandle_t rkResponseQueue;" in link_header
    source = code("App/tasks/rk/rk_task.c")
    assert "rk_link_next_sequence()" in source and "g_rk_seq_counter" not in source
    assert not re.search(r"extern\s+.*Queue", source)
    # 心跳随RK主循环推进，不能残留独立线程创建或失效的头文件引用。
    assert "process_heartbeat(&last_heartbeat);" in source
    for suffix in ("c", "h"):
        assert not (ROOT / f"App/tasks/rk/hb_task.{suffix}").exists()
    assert "StartHbTask" not in code("Core/Src/freertos.c")
    assert not (ROOT / "App/protocol/rk/dispatch.c").exists()
    passed += 1

    # B2有自己的任务/事务/解析状态，不借用RK串口、Seq、队列或最终结果处理。
    for base in ("App/tasks/b2", "App/protocol/b2"):
        for path in (ROOT / base).glob("*.[ch]"):
            source = code(str(path.relative_to(ROOT)))
            assert not re.search(r'\brk\w*|rk/|tasks/rk', source), path
    assert 'xTaskCreate(StartB2Task, "b2"' in code("Core/Src/freertos.c")
    assert "StartB2Task" not in code("App/tasks/rk/rk_task.c")
    assert (ROOT / "App/protocol/common/crc.c").exists()
    assert not (ROOT / "App/protocol/rk/crc.c").exists()
    passed += 1

    router = code("HAL_Drv/uart_router.c")
    hardware = code("HAL_Drv/uart_hw.c")
    assert router.index("uart_tx_mutex_init();") < router.index("uart_tx_isr_attach();")
    assert "ring == NULL || ring->mutex == NULL" in hardware
    assert "xSemaphoreTake(ring->mutex" in hardware
    assert "__set_PRIMASK(saved_primask);" in hardware
    assert "__enable_irq()" not in hardware
    assert "was_empty" not in hardware
    publish = hardware[hardware.index("static int queue_send("):]
    assert publish.index("ring->data[tail] = buffer[index];") < publish.index("ring->tail = tail;")
    assert publish.index("ring->tracked_end = tail;") < publish.index("ring->tail = tail;")
    assert "UART_TX_TRACKED_DRAINING" in publish
    passed += 1

    assert "id >= 0 && id < UART_SLOT_COUNT" in router
    assert "interface->start_rx == NULL" in router and "interface->send == NULL" in router
    assert "if (queue == NULL)" in router
    assert '__attribute__((section(".uart_dma")))' in router
    # 链接脚本的.uart_dma为NOLOAD；槽位指针必须在普通.bss内启动清零。
    assert "static uart_slot_t s_slots[UART_SLOT_COUNT];" in router
    assert re.search(r"s_rx_buffers\[UART_SLOT_COUNT\]\[UART_RX_BUF_SIZE\]\s*"
                     r'__attribute__\(\(section\("\.uart_dma"\)\)\)', router)
    assert "s_slots[id].rx_buffer" not in router
    passed += 1

    i2c = code("HAL_Drv/i2c_hw.c")
    spi = code("HAL_Drv/spi_hw.c")
    assert "device_address <= 0x7FU" in i2c
    assert "bus != NULL && bus->handle != NULL" in i2c
    assert "buffer == NULL || length == 0U" in i2c
    assert "return i2c_write_bytes(bus, device_address, &command, 1U);" in i2c
    assert "I2C_TRANSFER_TIMEOUT_MS 100U" in i2c
    assert "SPI_TRANSFER_TIMEOUT_MS 100U" in spi
    assert "bus == NULL || bus->handle == NULL" in spi
    assert "rx_buffer == NULL || length == 0U" in spi
    passed += 1

    monitor = code("App/tasks/common/monitor_task.c")
    assert "memory_monitor_init(&s_monitor)" in monitor
    assert ".pac_ready" not in monitor and ".adc_ready" not in monitor
    assert "snapshot.power_state == PWR_CTRL_STATE_ON" in monitor
    assert "memory_monitor_invalidate" in monitor
    passed += 1

    # 保留既定架构：PC仍通过FRU加载路径获取信息，不直读编译期版本。
    dispatch = code("App/protocol/pc/pc_dispatch.c")
    assert "pc_tester_info_load(&fru)" in dispatch
    assert "firmware_version_current" not in dispatch
    assert not re.search(r"\bswitch\s*\(", dispatch)
    assert "App/tasks/rk/rk_link.c" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    passed += 1

    # 枚举ISR排空与生产者“检查/发布/唤醒”的交错。只验证策略模型，
    # 不声称运行了真实中断/寄存器实现；旧策略至少有一个可复现反例。
    old_failures = 0
    for initial_bytes in (0, 8):
        for drain_position in range(4):
            events = ["check", "publish", "kick"]
            events.insert(drain_position, "drain")
            for unconditional_kick in (False, True):
                queued = initial_bytes
                txe_enabled = queued > 0
                was_empty = False
                for event in events:
                    if event == "check":
                        was_empty = queued == 0
                    elif event == "publish":
                        queued += 4
                    elif event == "kick":
                        if unconditional_kick or was_empty:
                            txe_enabled = True
                    elif event == "drain" and txe_enabled:
                        queued = 0
                        txe_enabled = False
                stalled = queued > 0 and not txe_enabled
                if unconditional_kick:
                    assert not stalled, events
                else:
                    old_failures += int(stalled)
            passed += 1
    assert old_failures > 0
    passed += 1
    print(f"Code boundaries / event model: {passed} PASS, 0 FAIL (not C runtime)")


if __name__ == "__main__":
    main()
