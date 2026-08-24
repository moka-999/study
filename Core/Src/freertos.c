/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "queue.h"
#include "tim.h"
#include "uart_router.h"
#include "hal_if.h"
#include "wdt.h"
#include "iwdg.h"
#include "led.h"
#include "qspi_hw.h"
#include "frame.h"
#include "dispatch.h"
#include "hb_task.h"
#include "uart_router.h"
#include "pwr_ctrl.h"
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
/* USER CODE BEGIN Variables */
TaskHandle_t      ledBlinkTaskHandle;
TaskHandle_t      tcpClientTaskHandle;
TaskHandle_t      monitorTaskHandle;
QueueHandle_t     monitorQueue;             /* 监测→TCP 消息队列 */
QueueHandle_t     rkToPc_Queue;             /* RK→PC 上报队列     */
QueueHandle_t     pcToRk_Queue;             /* PC→RK 转发队列     */
char              mon_msg[128];            /* 监测报文缓冲(非static, tcp_task用) */
/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 2048 * 4,   /* 8KB: MX_LWIP_Init 需大栈, 128*4=512B 会栈溢出(2026-08-13 实测) */
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void StartLedBlinkTask(void *argument);
void StartTcpServerTask(void *argument);
void StartMonitorTask(void *argument);
void StartRkTask(void *argument);
void StartHbTask(void *argument);
void StartWdtFeedTask(void *argument);
/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

extern void MX_LWIP_Init(void);
void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
#if !defined(ENABLE_FLASH_SELF_TEST) || !ENABLE_FLASH_SELF_TEST
  /* 注册 UART 接口 (先不创建 mutex, 避免堆碎片化) */
  extern uart_if_t uart1_if, uart2_if;
  uart_register(0, &uart1_if);
  uart_register(1, &uart2_if);
  uart_start_all();
#endif
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
#if !defined(ENABLE_FLASH_SELF_TEST) || !ENABLE_FLASH_SELF_TEST
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);
#endif

  /* USER CODE BEGIN RTOS_THREADS */
#if defined(ENABLE_FLASH_SELF_TEST) && ENABLE_FLASH_SELF_TEST
  /*
   * W25Q256硬件自检镜像只保留自检和喂狗任务，避免网关任务占用USART1，
   * 也避免任何非Flash业务影响串口日志与复位持久化判断。
   */
  extern void StartQspiTestTask(void *argument);
  xTaskCreate(StartQspiTestTask, "qspiTest", 2048, NULL, 3, NULL);
  xTaskCreate(StartWdtFeedTask, "wdtFeed", 128, NULL, 25, NULL);
#else
  /* creation of ledBlinkTask */
  xTaskCreate(StartLedBlinkTask, "ledBlinkTask", 128, NULL, 1, &ledBlinkTaskHandle);
  /* TCP server — 完整 socket+TLV 协议需 sm(1056)+rxbuf(1024)+frame(1172)+resp(1040)
     ≈峰值6KB, 4KB(1024字)不够, 加大到 16KB(4096字) */
  xTaskCreate(StartTcpServerTask, "tcp", 4096, NULL, 5, &tcpClientTaskHandle);
  /* 监测任务 */
  xTaskCreate(StartMonitorTask, "monitor", 256, NULL, 2, &monitorTaskHandle);
  monitorQueue = xQueueCreate(4, 128);
  /* rk 任务: 栈上 6.5KB 大缓冲(buf+sm+frame+msg+ack+fwd), 8KB 会溢出踩 TCB,
     加大到 32KB(8192字) */
  xTaskCreate(StartRkTask, "rk", 8192, NULL, 3, NULL);
  /* hb 任务: hb[TLV_FRAME_MAX]=1040B 单这一项就超 1KB, 加大到 4KB(1024字) */
  xTaskCreate(StartHbTask, "hb", 1024, NULL, 3, NULL);
  rkToPc_Queue  = xQueueCreate(8, sizeof(rk_msg_t));   /* RK→PC 上报队列     */
  pcToRk_Queue  = xQueueCreate(8, sizeof(pc2rk_msg_t)); /* PC→RK 命令转发队列 */
  /* 看门狗喂狗: 优先级 25, 高于 defaultTask(24) 确保 MX_LWIP_Init 阻塞时仍能喂狗 */
  xTaskCreate(StartWdtFeedTask, "wdtFeed", 128, NULL, 25, NULL);





  
#if defined(ENABLE_DRIVER_SELF_TEST) && ENABLE_DRIVER_SELF_TEST
  /* 驱动自检仅用于开发镜像，生产构建默认不创建。 */
  extern void StartTestTask(void *argument);
  xTaskCreate(StartTestTask, "drvTest", 1024, NULL, 3, NULL);
#endif
#if defined(ENABLE_PWR_WAVE_TEST) && ENABLE_PWR_WAVE_TEST
  /*
   * V2 电源波形镜像只创建这一条轻量测试任务。
   * 正式构建未定义 ENABLE_PWR_WAVE_TEST，因此不会改变生产启动行为。
   */
  extern void StartPwrWaveTask(void *argument);
  xTaskCreate(StartPwrWaveTask, "pwrWave", 256, NULL, 2, NULL);
#endif
#if defined(ENABLE_RESULT_LED_WAVE_TEST) && ENABLE_RESULT_LED_WAVE_TEST
  /* 结果灯测试镜像使用六路替代GPIO，正式构建不会创建该任务。 */
  extern void StartResultLedWaveTask(void *argument);
  xTaskCreate(StartResultLedWaveTask, "resultLedWave", 256, NULL, 2, NULL);
#endif
#endif /* ENABLE_FLASH_SELF_TEST */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
#if !defined(ENABLE_FLASH_SELF_TEST) || !ENABLE_FLASH_SELF_TEST
  /* init code for LWIP */
  MX_LWIP_Init();
#endif
  /* USER CODE BEGIN StartDefaultTask */
  /* Infinite loop */
  for(;;)
  {
    /* 非阻塞推进电源状态机；未配置时保持 OFF，不产生 GPIO 翻转。 */
    pwr_ctrl_process(HAL_GetTick());
    osDelay(1);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
/* 任务实现在 App/tasks/ 下 */

/* 看门狗喂狗任务: 最高优先级, 2s 周期 */
void StartWdtFeedTask(void *argument)
{
  (void)argument;
  for (;;) {
    wdt_feed();
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

/* 栈溢出钩子: 写入 crash_log[8..11], PB1 常亮=溢出, 然后停机 */
extern uint32_t crash_log[16];
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  crash_log[8]  = 0x57000001;          /* 0x57 = "STack" */
  crash_log[9]  = (uint32_t)xTask;     /* 溢出的任务句柄 */
  crash_log[10] = (uint32_t)pcTaskName; /* 前4字节任务名 */
  crash_log[11] = 0;                   /* placeholder */
  led_on(LED_CONNECT);  /* PB1 常亮 = 栈溢出 */
  taskDISABLE_INTERRUPTS();
  for(;;) {}
}
/* USER CODE END Application */

