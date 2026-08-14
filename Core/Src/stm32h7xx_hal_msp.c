/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file         stm32h7xx_hal_msp.c
  * @brief        This file provides code for the MSP Initialization
  *               and de-Initialization codes.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
/* USER CODE BEGIN Includes */
#include "pcf8574.h"
#include "led.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN Define */

/* USER CODE END Define */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN Macro */

/* USER CODE END Macro */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* External functions --------------------------------------------------------*/
/* USER CODE BEGIN ExternalFunctions */

/* USER CODE END ExternalFunctions */

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */
/**
  * Initializes the Global MSP.
  */
void HAL_MspInit(void)
{

  /* USER CODE BEGIN MspInit 0 */

  /* USER CODE END MspInit 0 */

  __HAL_RCC_SYSCFG_CLK_ENABLE();

  /* System interrupt init*/
  /* PendSV_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(PendSV_IRQn, 15, 0);

  /* USER CODE BEGIN MspInit 1 */

  /* USER CODE END MspInit 1 */
}

/* USER CODE BEGIN 1 */

/**
  * @brief  ETH MSP 初始化 —— 使能 ETH 外设的 GPIO 时钟和复用功能
  * @note   HAL_ETH_Init() 内部会调用此函数，CubeMX 生成的弱定义是空的，必须覆写
  */
void HAL_ETH_MspInit(ETH_HandleTypeDef *heth)
{
  GPIO_InitTypeDef gpio = {0};

  /* ── 使能 GPIO 时钟 ── */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /* ── 使能 ETH MAC 和 DMA 时钟 ── */
  __HAL_RCC_ETH1MAC_CLK_ENABLE();
  __HAL_RCC_ETH1TX_CLK_ENABLE();
  __HAL_RCC_ETH1RX_CLK_ENABLE();

  /* ── RMII 引脚复用配置 (AF11) ── */
  /* PA1: ETH_REF_CLK   */  gpio.Pin = GPIO_PIN_1;
  /* PA2: ETH_MDIO      */  gpio.Pin |= GPIO_PIN_2;
  /* PA7: ETH_CRS_DV    */  gpio.Pin |= GPIO_PIN_7;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_HIGH;
  gpio.Alternate = GPIO_AF11_ETH;
  HAL_GPIO_Init(GPIOA, &gpio);

  /* PC1: ETH_MDC       */  gpio.Pin = GPIO_PIN_1;
  /* PC4: ETH_RXD0      */  gpio.Pin |= GPIO_PIN_4;
  /* PC5: ETH_RXD1      */  gpio.Pin |= GPIO_PIN_5;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_HIGH;
  gpio.Alternate = GPIO_AF11_ETH;
  HAL_GPIO_Init(GPIOC, &gpio);

  /* PB11: ETH_TX_EN    */  gpio.Pin = GPIO_PIN_11;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_HIGH;
  gpio.Alternate = GPIO_AF11_ETH;
  HAL_GPIO_Init(GPIOB, &gpio);

  /* PG13: ETH_TXD0     */  gpio.Pin = GPIO_PIN_13;
  /* PG14: ETH_TXD1     */  gpio.Pin |= GPIO_PIN_14;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_HIGH;
  gpio.Alternate = GPIO_AF11_ETH;
  HAL_GPIO_Init(GPIOG, &gpio);

  /* ── PCF8574 初始化（I2C IO扩展芯片，控制PHY硬件复位） ── */
  {
    uint8_t pcf_ret = pcf8574_init();

    if (pcf_ret != 0) {
      /* ★ PCF8574 无应答 — PHY 复位失败 ★
       * LED PB1 快闪 3 次 → 停顿 → 循环 (SOS 模式) */
      while (1) {
        for (int i = 0; i < 3; i++) {
          led_on(LED_CONNECT);   HAL_Delay(150);
          led_off(LED_CONNECT);  HAL_Delay(150);
        }
        HAL_Delay(800);
      }
    }

    /* PCF8574 OK — 执行 PHY 硬件复位时序 */
    pcf8574_write_bit(ETH_RESET_IO, 0);   /* PHY 复位拉低 */
    HAL_Delay(100);
    pcf8574_write_bit(ETH_RESET_IO, 1);   /* PHY 释放复位 */
    HAL_Delay(100);

    /* 验证: 读回 P7 确认是高电平 */
    if (pcf8574_read_bit(ETH_RESET_IO) != 1) {
      /* ★ PHY 复位脚读回异常 ★ */
      while (1) {
        for (int i = 0; i < 5; i++) {
          led_on(LED_CONNECT);   HAL_Delay(100);
          led_off(LED_CONNECT);  HAL_Delay(100);
        }
        HAL_Delay(800);
      }
    }
  }

  /* ── ETH 中断 ── */
  HAL_NVIC_SetPriority(ETH_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(ETH_IRQn);
}

/**
  * @brief  ETH MSP 反初始化
  */
void HAL_ETH_MspDeInit(ETH_HandleTypeDef *heth)
{
  __HAL_RCC_ETH1MAC_CLK_DISABLE();
  __HAL_RCC_ETH1TX_CLK_DISABLE();
  __HAL_RCC_ETH1RX_CLK_DISABLE();

  HAL_GPIO_DeInit(GPIOA, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7);
  HAL_GPIO_DeInit(GPIOB, GPIO_PIN_11);
  HAL_GPIO_DeInit(GPIOC, GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5);
  HAL_GPIO_DeInit(GPIOG, GPIO_PIN_13 | GPIO_PIN_14);

  HAL_NVIC_DisableIRQ(ETH_IRQn);
}

/* USER CODE END 1 */
