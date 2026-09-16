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
#include "ota_app.h"
#include "flash_bsp.h"

/* 链接脚本为固定LwIP堆预留32KiB，其中需包含分配器元数据和对齐空间。 */
_Static_assert(MEM_SIZE <= 30U * 1024U, "Update B1 .lwip_heap reservation before growing MEM_SIZE");

/* 当前沿用V2内部排队模型；正式PC算法业务尚未接入，不代表双协议闭环。 */
QueueHandle_t pcToRk_Queue;
QueueHandle_t rkToPc_Queue;

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
    MX_LWIP_Init();

    for (;;) {
        /* 500ms 结果灯状态机必须非阻塞推进，后续网关任务才能并行运行。 */
        result_led_process(HAL_GetTick());
        monitor_power_process(HAL_GetTick());
        ota_app_progress(OTA_TASK_DEFAULT);
        osDelay(1U);
    }
}

#if defined(ENABLE_OTA_BOOT) && ENABLE_OTA_BOOT
/* 完整B已启动IWDG；每500ms检查五个任务，失败不喂狗，由B累计启动失败。
 * 不是单独检查RK在线；OFF时各任务仍须上报自身正常运行。使用4KiB栈写日志。 */
static void StartOtaWatchdogTask(void *argument)
{
    (void)argument;
    for (;;) {
        if (ota_app_watchdog_tick()) IWDG1->KR = 0xAAAAU;
        vTaskDelay(pdMS_TO_TICKS(500U));
    }
}
#endif

void MX_FREERTOS_Init(void)
{
    /* cJSON全局分配器必须在任何任务创建前切换到AXI SRAM中的FreeRTOS堆。 */
    pc_json_configure_allocator(pvPortMalloc, vPortFree);
    if (flash_transaction_prepare() != 0) Error_Handler();
    ota_app_prepare();

    pcToRk_Queue = xQueueCreate(8, sizeof(pc2rk_msg_t));
    rkToPc_Queue = xQueueCreate(8, sizeof(rk_msg_t));
    if (!pcToRk_Queue || !rkToPc_Queue || !monitor_task_prepare())
        Error_Handler();

    /* UART4为软件暂选值，实机确认后用CMake B1_RK_UART=2切换USART2。
     * 同一次构建只向slot0注册一路，不把本地USART1或第二候选口当网关。 */
#if B1_RK_UART == 4
    uart_register(0, &uart4_if);
#elif B1_RK_UART == 2
    uart_register(0, &uart2_if);
#else
#error "B1_RK_UART must be 4 (UART4) or 2 (USART2)"
#endif

    if (osThreadNew(StartDefaultTask, NULL, &s_default_task_attributes) == NULL) {
        Error_Handler();
    }
    /* xTaskCreate的栈单位是StackType_t，以下分别为16/32/4/2KiB。 */
    if (xTaskCreate(StartTcpServerTask, "tcp", 4096, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(StartRkTask, "rk", 8192, NULL, 3, NULL) != pdPASS ||
        xTaskCreate(StartHbTask, "hb", 1024, NULL, 3, NULL) != pdPASS ||
        xTaskCreate(StartMonitorTask, "monitor", 512, NULL, 2, NULL) != pdPASS) {
        Error_Handler();
    }
#if defined(ENABLE_OTA_BOOT) && ENABLE_OTA_BOOT
    if (xTaskCreate(StartOtaWatchdogTask, "otaWatchdog", 1024, NULL, 25, NULL) != pdPASS)
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
