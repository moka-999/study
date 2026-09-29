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
#include "rk_link.h"
#include "uart_router.h"
#include "pwr_ctrl.h"
#include "pc_protocol.h"
#include "monitor_task.h"
#include "result_led.h"
#include "ota_app.h"
#include "flash_bsp.h"
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
QueueHandle_t     rkRequestQueue;          /* 业务层→RK的结构化请求。 */
QueueHandle_t     rkResponseQueue;         /* RK→业务层的完整结果。 */
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

  /*
   * cJSON 的分配器是全局配置，必须在创建任务前一次性切换到 FreeRTOS 堆。
   * 若使用默认 libc malloc，内存不会计入 xPortGetFreeHeapSize()，也不符合
   * 当前固件统一放置在 AXI SRAM 的堆规划。
   */
  pc_json_configure_allocator(pvPortMalloc, vPortFree);
  if (flash_transaction_prepare() != 0) Error_Handler();
  ota_app_prepare();
#if (!defined(ENABLE_FLASH_SELF_TEST) || !ENABLE_FLASH_SELF_TEST) && \
    (!defined(ENABLE_DRIVER_MOCK_TEST) || !ENABLE_DRIVER_MOCK_TEST) && \
    (!defined(ENABLE_AD7124_WAVE_TEST) || !ENABLE_AD7124_WAVE_TEST)
  if (!monitor_task_prepare())
    Error_Handler();
#endif

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
#if (!defined(ENABLE_FLASH_SELF_TEST) || !ENABLE_FLASH_SELF_TEST) && \
    (!defined(ENABLE_DRIVER_MOCK_TEST) || !ENABLE_DRIVER_MOCK_TEST) && \
    (!defined(ENABLE_AD7124_WAVE_TEST) || !ENABLE_AD7124_WAVE_TEST)
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
#elif defined(ENABLE_DRIVER_MOCK_TEST) && ENABLE_DRIVER_MOCK_TEST
  /*
   * 事务级Mock镜像不创建网络、RK或真实器件自检任务，保证结果只来自
   * TPS/PAC/AD/MCP正式驱动代码和内存Mock接口。
   */
  extern void StartDriverMockTestTask(void *argument);
  xTaskCreate(StartDriverMockTestTask, "drvMock", 2048, NULL, 3, NULL);
  xTaskCreate(StartWdtFeedTask, "wdtFeed", 128, NULL, 25, NULL);
#elif defined(ENABLE_AD7124_WAVE_TEST) && ENABLE_AD7124_WAVE_TEST
  /*
   * AD7124波形镜像只运行SPI2测试任务和喂狗任务，避免网络及其他驱动
   * 占用CPU或在采集窗口内产生无关活动。
   */
  extern void StartAd7124WaveTask(void *argument);
  xTaskCreate(StartAd7124WaveTask, "adWave", 512, NULL, 3, NULL);
  xTaskCreate(StartWdtFeedTask, "wdtFeed", 128, NULL, 25, NULL);
#elif defined(ENABLE_PC_PROTOCOL_NET_TEST) && ENABLE_PC_PROTOCOL_NET_TEST
  /*
   * PC协议网络验收镜像保留LWIP、TCP、缓存监测和结果灯业务，不启动RK
   * 或自主波形任务，确保控制只来自18字节头+cJSON请求。
   */
  xTaskCreate(StartTcpServerTask, "pcNetTest", 4096, NULL, 5,
              &tcpClientTaskHandle);
  xTaskCreate(StartMonitorTask, "monitor", 512, NULL, 2, &monitorTaskHandle);
  xTaskCreate(StartWdtFeedTask, "wdtFeed", 128, NULL, 25, NULL);
#else
  /* 创建板载运行指示任务。 */
  xTaskCreate(StartLedBlinkTask, "ledBlinkTask", 128, NULL, 1, &ledBlinkTaskHandle);
  /*
   * PC TCP server栈设为16KiB（4096个StackType_t）：任务栈上同时存在4KiB
   * pc_stream、1KiB recv缓冲以及JSON解析/响应临时对象。正式TCP已不再创建
   * RK TLV状态机；保留余量用于lwIP socket调用和后续业务适配层。
   */
  xTaskCreate(StartTcpServerTask, "tcp", 4096, NULL, 5, &tcpClientTaskHandle);
  /* 监测任务包含AD/PAC采集栈帧，保留2KiB栈空间。 */
  xTaskCreate(StartMonitorTask, "monitor", 512, NULL, 2, &monitorTaskHandle);
  monitorQueue = xQueueCreate(4, 128);
  /* RK任务栈保留32KiB，容纳RX块、状态机及命令/心跳组帧，统一推进收发。 */
  xTaskCreate(StartRkTask, "rk", 8192, NULL, 3, NULL);
  rkRequestQueue = xQueueCreate(8, sizeof(rk_request_t));
  rkResponseQueue = xQueueCreate(8, sizeof(rk_response_t));
  if (rkRequestQueue == NULL || rkResponseQueue == NULL) Error_Handler();
  /* OTA确认需读写256B日志及校验缓冲，留4KiB栈；不再无条件给卡死任务喂狗。 */
  xTaskCreate(StartWdtFeedTask, "wdtFeed", 1024, NULL, 25, NULL);





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
    monitor_power_process(HAL_GetTick());
    ota_app_progress(OTA_TASK_DEFAULT);
#if defined(ENABLE_V2_RESULT_LED) && ENABLE_V2_RESULT_LED
    /* 网络命令改变业务状态，本任务只负责推进共享500ms闪烁相位。 */
    result_led_process(HAL_GetTick());
#endif
    osDelay(1);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
/* 正式任务位于App/tasks/{pc,rk,common}，隔离验证任务位于App/tasks/test。 */

/* 最高优先级每500ms检查关键任务进展；OTA失去健康后停止喂狗，交B统计失败。 */
void StartWdtFeedTask(void *argument)
{
  (void)argument;
  for (;;) {
    if (ota_app_watchdog_tick()) wdt_feed();
    vTaskDelay(pdMS_TO_TICKS(500));
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

