#include "main.h"

/*
 * B1 的 RMII 复用引脚由 MX_GPIO_Init() 统一配置；本文件只负责 ETH MAC
 * 时钟和中断。ETH 中断会释放 LwIP 信号量，因此优先级不得高于
 * FreeRTOS 可调用系统 API 的上限（数值不得小于 5）。
 */
void HAL_ETH_MspInit(ETH_HandleTypeDef *heth)
{
    if (heth->Instance != ETH) return;

    __HAL_RCC_ETH1MAC_CLK_ENABLE();
    __HAL_RCC_ETH1TX_CLK_ENABLE();
    __HAL_RCC_ETH1RX_CLK_ENABLE();
    HAL_NVIC_SetPriority(ETH_IRQn, 5U, 0U);
    HAL_NVIC_EnableIRQ(ETH_IRQn);
}

void HAL_ETH_MspDeInit(ETH_HandleTypeDef *heth)
{
    if (heth->Instance != ETH) return;

    HAL_NVIC_DisableIRQ(ETH_IRQn);
    __HAL_RCC_ETH1RX_CLK_DISABLE();
    __HAL_RCC_ETH1TX_CLK_DISABLE();
    __HAL_RCC_ETH1MAC_CLK_DISABLE();
}
