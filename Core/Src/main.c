/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "dma.h"
#include "i2c.h"
#include "iwdg.h"
#include "lwip.h"
#include "quadspi.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "pwr_ctrl.h"
#include "pc_power_service.h"
#include "result_led.h"
#if defined(ENABLE_RESULT_LED_WAVE_TEST) && ENABLE_RESULT_LED_WAVE_TEST
extern void result_led_wave_early_init(void);
#endif

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#if defined(ENABLE_V2_RESULT_LED) && ENABLE_V2_RESULT_LED
/*
 * V2没有B1的PG0-PG5结果灯网络，正式/网络镜像复用已通过波形验收的六路排针：
 * PD7/PD3、PD2/PC12、PC11/PC10分别模拟LPDDR0、LPDDR1、eMCP的FAIL/PASS。
 * 这些引脚可能与摄像头、LCD、SD扩展复用，运行本镜像时不得连接对应模块。
 */
static const result_led_board_map_t s_v2_result_led_map = {
  .pairs = {
    [RESULT_LED_LPDDR0] = {
      .fail_port = GPIOD, .fail_pin = GPIO_PIN_7,
      .pass_port = GPIOD, .pass_pin = GPIO_PIN_3,
    },
    [RESULT_LED_LPDDR1] = {
      .fail_port = GPIOD, .fail_pin = GPIO_PIN_2,
      .pass_port = GPIOC, .pass_pin = GPIO_PIN_12,
    },
    [RESULT_LED_EMCP] = {
      .fail_port = GPIOC, .fail_pin = GPIO_PIN_11,
      .pass_port = GPIOC, .pass_pin = GPIO_PIN_10,
    },
  },
};

static void v2_result_led_init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;

  for (uint32_t target = 0U; target < RESULT_LED_TARGET_COUNT; target++) {
    const result_led_pin_pair_t *pair = &s_v2_result_led_map.pairs[target];
    HAL_GPIO_WritePin(pair->fail_port, pair->fail_pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(pair->pass_port, pair->pass_pin, GPIO_PIN_RESET);
    gpio.Pin = pair->fail_pin;
    HAL_GPIO_Init(pair->fail_port, &gpio);
    gpio.Pin = pair->pass_pin;
    HAL_GPIO_Init(pair->pass_port, &gpio);
  }
  if (result_led_init(&s_v2_result_led_map) != HAL_OK)
    Error_Handler();
}
#endif

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_I2C1_Init();
  MX_SPI2_Init();
  MX_TIM8_Init();
  MX_USART2_UART_Init();
  MX_I2C3_Init();
  MX_USART3_UART_Init();
  MX_I2C4_Init();
  MX_QUADSPI_Init();
  /* USER CODE BEGIN 2 */
  /* 外设初始化后开启看门狗，运行期由任务存活监测统一喂狗。 */
  MX_IWDG1_Init();
#if defined(ENABLE_RESULT_LED_WAVE_TEST) && ENABLE_RESULT_LED_WAVE_TEST
  /*
   * 在调度器和LWIP启动前关闭六路替代结果灯。若等低优先级测试任务运行，
   * 部分高阻引脚会被Apollo V2板上外部网络拉高，形成数秒假亮窗口。
   */
  result_led_wave_early_init();
#endif
#if defined(ENABLE_V2_RESULT_LED) && ENABLE_V2_RESULT_LED
  /* 正式/网络镜像在任务启动前建立全灭基线和三组替代GPIO映射。 */
  v2_result_led_init();
#endif
  /*
   * 未确认 PC6/PC7 真板时序前不加载默认配置，电源保持安全关闭。
   * 板级联调在此提供pwr_ctrl_config_t及pc_power_config_t/RK确认适配器；
   * 两层配置分别控制轨间时序和业务等待，接入方法见doc/power_sequence_20261012.md。
   */
  pwr_ctrl_init(NULL);
  (void)pc_power_service_configure(NULL, NULL);

  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 50;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM1 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM1)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
