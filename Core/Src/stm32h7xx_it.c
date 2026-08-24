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
