#include "main.h"

extern ETH_HandleTypeDef heth;
extern TIM_HandleTypeDef htim1;

void NMI_Handler(void) {}
void HardFault_Handler(void) { while (1) {} }
void MemManage_Handler(void) { while (1) {} }
void BusFault_Handler(void) { while (1) {} }
void UsageFault_Handler(void) { while (1) {} }
void DebugMon_Handler(void) {}

/* HAL 的 1ms 时间基准独占 TIM1；SysTick 留给 FreeRTOS 内核。 */
void TIM1_UP_IRQHandler(void) { HAL_TIM_IRQHandler(&htim1); }

/* ETH DMA 中断只交给 HAL，完成回调再唤醒 LwIP 收发线程。 */
void ETH_IRQHandler(void) { HAL_ETH_IRQHandler(&heth); }

/* 两路候选RK UART的接收DMA/IDLE与TXE统一交给HAL钩子处理。 */
void DMA1_Stream0_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_uart4_rx); }
void DMA1_Stream1_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_usart2_rx); }
void UART4_IRQHandler(void) { HAL_UART_IRQHandler(&huart4); }
void USART2_IRQHandler(void) { HAL_UART_IRQHandler(&huart2); }
