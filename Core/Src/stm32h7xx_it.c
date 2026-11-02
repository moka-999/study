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

/* TIM1每1ms推进HAL计时；其他定时器回调不得改变HAL_Delay的时间基准。 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM1) {
        HAL_IncTick();
    }
}

/* ETH DMA 中断只交给 HAL，完成回调再唤醒 LwIP 收发线程。 */
void ETH_IRQHandler(void) { HAL_ETH_IRQHandler(&heth); }

/* 协议、RK日志与本地控制台的独立DMA/IDLE/TXE统一交给HAL钩子。 */
void DMA1_Stream0_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_uart4_rx); }
void DMA1_Stream1_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_usart2_rx); }
void DMA1_Stream2_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_usart1_rx); }
void UART4_IRQHandler(void) { HAL_UART_IRQHandler(&huart4); }
void USART2_IRQHandler(void) { HAL_UART_IRQHandler(&huart2); }
void USART1_IRQHandler(void) { HAL_UART_IRQHandler(&huart1); }
