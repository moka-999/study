"""运行实际UART发送环及TC完成钩子的C回归；只模拟寄存器与RTOS，不连接板卡。"""
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
    build = root / 'build/uart-tx-test'
    build.mkdir(parents=True, exist_ok=True)
    (build / 'port.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { volatile uint32_t ISR, CR1, TDR; } USART_TypeDef;
typedef struct { struct { uint32_t Mode; } Init; } DMA_HandleTypeDef;
typedef struct UART_HandleTypeDef {
    USART_TypeDef *Instance;
    DMA_HandleTypeDef *hdmarx;
    void (*TxISR)(struct UART_HandleTypeDef *);
} UART_HandleTypeDef;
extern UART_HandleTypeDef huart1, huart2, huart4;
typedef unsigned TickType_t;
typedef int *SemaphoreHandle_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define USART_ISR_TXE_TXFNF 1U
#define USART_CR1_TXEIE 2U
#define UART_IT_TXE USART_CR1_TXEIE
#define UART_IT_TC 4U
#define HAL_OK 0
#define DMA_NORMAL 0
#define DMA_IT_HT 1U
#define __HAL_UART_ENABLE_IT(uart, flags) ((uart)->Instance->CR1 |= (flags))
#define __HAL_DMA_DISABLE_IT(dma, flags) ((void)(dma), (void)(flags))
uint32_t HAL_GetTick(void);
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
int HAL_UART_AbortReceive(UART_HandleTypeDef *);
int HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef *, uint8_t *, uint16_t);
''', encoding='utf-8')
    for header in ('main', 'usart', 'FreeRTOS', 'semphr', 'board_interfaces'):
        (build / f'{header}.h').write_text('#include "port.h"\n', encoding='utf-8')
    (build / 'uart_under_test.h').write_text(
        '#include "' + (root / 'HAL_Drv/uart_hw.c').as_posix() + '"\n', encoding='utf-8')
    support = Path(__file__).resolve().parent / 'support'
    command = [args.compiler, '-mcpu=cortex-m7', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-d16',
               '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections',
               '-I' + str(build), '-I' + str(root / 'HAL_Drv'), str(Path(__file__).with_name('test_uart_tx.c')),
               str(support / 'qemu_start.s'), '-nostartfiles', '--specs=nosys.specs',
               '-T' + str(support / 'qemu_test.ld'), '-Wl,--gc-sections', '-o', str(build / 'test.elf')]
    if (root / "b1.ioc").exists():
        command += ["-DTEST_B1=1"]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (build / 'build.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    result = subprocess.run([args.qemu, '-M', 'mps2-an500', '-display', 'none', '-monitor', 'none',
                             '-serial', 'none', '-no-reboot', '-semihosting-config', 'enable=on,target=native',
                             '-kernel', str(build / 'test.elf')], capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / 'run.log').write_text(output, encoding='utf-8')
    if result.returncode or 'PASS: UART tracked TC' not in output:
        raise RuntimeError(output)
    print(f'{root.name}: {output.strip()}')


if __name__ == '__main__':
    main()
