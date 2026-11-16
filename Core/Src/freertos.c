#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "lwip.h"
#include "main.h"
#include "pc_protocol.h"
#include "result_led.h"
#include "task.h"
#include "queue.h"
#include "board_interfaces.h"
#include "uart_router.h"
#include "rk_link.h"
#include "app_tasks.h"
#include "monitor_task.h"
#include "pwr_ctrl.h"
#include "lwipopts.h"
#include "task_watchdog.h"
#include "board_watchdog_config.h"
#include "flash_bsp.h"
#include "log_console.h"
#include "net_console.h"
#include "board_diagnostics.h"
#include "board_identity.h"
#include "b2_service.h"
#include "b2_task.h"
#include <stdio.h>
#include "handoff_mcu.h"

/* 链接脚本为固定LwIP堆预留32KiB，其中需包含分配器元数据和对齐空间。 */
_Static_assert(MEM_SIZE <= 30U * 1024U, "Update B1 .lwip_heap reservation before growing MEM_SIZE");

/* RK串行执行，业务层通过完整请求/结果副本交换，不传递PC帧。 */
QueueHandle_t rkRequestQueue;
QueueHandle_t rkResponseQueue;

/*
 * 网络初始化会创建 tcpip、收包和链路监测线程，512B 的 CubeMX 默认栈
 * 不够使用。这里沿用 V2 实测稳定的 8KiB，并将 LwIP 初始化放在线程
 * 上下文中，禁止在调度器启动前调用任何依赖 RTOS 的接口。
 */
static const osThreadAttr_t s_default_task_attributes = {
    .name = "defaultTask",
    .stack_size = 8U * 1024U,
    .priority = osPriorityNormal,
};

static void StartDefaultTask(void *argument)
{
    (void)argument;
    /* 队列已在调度前注册；在调度后开启DMA，FromISR才有可用的内核上下文。 */
    uart_start_all();
    log_console_start();
    static const char handoff_banner[] = "[handoff] H1C same-IP/4M; full B1; OFF: net rk, then start rk\r\n";
    (void)log_console_write(handoff_banner, sizeof(handoff_banner) - 1U);
    static const char network_start[] = "[boot] lwIP init start\r\n";
    static const char network_done[] = "[boot] lwIP init returned; link requires status check\r\n";
    (void)log_console_write(network_start, sizeof(network_start) - 1U);
    MX_LWIP_Init();
    net_console_start();
    (void)log_console_write(network_done, sizeof(network_done) - 1U);

    /* 只表示MCU已完成启动初始化，不表示RK/DDR上电、网络连通或测试PASS。 */
    HAL_GPIO_WritePin(B1_LED7_PORT, B1_LED7_PIN, GPIO_PIN_SET);
    printf("B1 slot code: %u (PB5..PB3)\r\n", (unsigned int)board_slot_code());

    for (;;) {
        /* 500ms 结果灯状态机必须非阻塞推进，后续网关任务才能并行运行。 */
        result_led_process(HAL_GetTick());
        monitor_power_process(HAL_GetTick());
        log_console_process();
        net_console_process();
        board_diagnostics_process();
        task_watchdog_progress(TASK_WATCHDOG_DEFAULT);
        osDelay(1U);
    }
}

#if BOARD_IWDG_ENABLE
/* IWDG由main启动；每500ms检查必要任务进展，超期停止喂狗。
 * OFF时也监督RK任务自身调度，不要求RK设备实际在线。 */
static void StartWatchdogTask(void *argument)
{
    (void)argument;
    for (;;) {
        if (task_watchdog_can_feed()) IWDG1->KR = 0xAAAAU;
        vTaskDelay(pdMS_TO_TICKS(500U));
    }
}
#endif

void MX_FREERTOS_Init(void)
{
    /* cJSON全局分配器必须在任何任务创建前切换到AXI SRAM中的FreeRTOS堆。 */
    pc_json_configure_allocator(pvPortMalloc, vPortFree);
    if (flash_transaction_prepare() != 0) Error_Handler();
#if BOARD_IWDG_ENABLE
    task_watchdog_prepare(TASK_WATCHDOG_ALL);
#endif
    if (!b2_service_prepare() || !handoff_mcu_prepare()) Error_Handler();

    rkRequestQueue = xQueueCreate(8, sizeof(rk_request_t));
    rkResponseQueue = xQueueCreate(8, sizeof(rk_response_t));
    if (!rkRequestQueue || !rkResponseQueue || !monitor_task_prepare())
        Error_Handler();

    /* 三路独立接收队列；只有slot0进入RK协议，另外两路只交给本地日志模块。 */
    uart_register(UART_PORT_RK_PROTOCOL, &uart4_if);
    uart_register(UART_PORT_RK_DEBUG, &uart2_if);
    uart_register(UART_PORT_CONSOLE, &uart1_if);
    uart_rx_diagnostics_t uart_diagnostics;
    for (int port = 0; port < UART_PORT_COUNT; ++port) {
        if (!uart_get_rx_diagnostics(port, &uart_diagnostics)) Error_Handler();
    }

    if (osThreadNew(StartDefaultTask, NULL, &s_default_task_attributes) == NULL) {
        Error_Handler();
    }
    /* 栈单位为StackType_t，TCP/RK/监测分别16/32/2KiB；RK只处理自己的串口链路。 */
    if (xTaskCreate(StartTcpServerTask, "tcp", 4096, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(StartRkTask, "rk", 8192, NULL, 3, NULL) != pdPASS ||
        xTaskCreate(StartMonitorTask, "monitor", 512, NULL, 2, NULL) != pdPASS) {
        Error_Handler();
    }
    /* B2独立SPI任务使用4KiB栈，不进入RK的队列、流水号或结果处理。 */
    if (xTaskCreate(StartB2Task, "b2", 1024, NULL, 4, NULL) != pdPASS) Error_Handler();
#if BOARD_IWDG_ENABLE
    if (xTaskCreate(StartWatchdogTask, "watchdog", 256, NULL, 25, NULL) != pdPASS)
        Error_Handler();
#endif
}

/* 栈溢出表示内存已可能损坏，立即停机，避免继续控制电源或发送错帧。 */
void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    (void)task_name;
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}
