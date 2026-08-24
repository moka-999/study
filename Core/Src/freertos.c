#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "lwip.h"
#include "main.h"
#include "result_led.h"
#include "task.h"

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
    MX_LWIP_Init();

    for (;;) {
        /* 500ms 结果灯状态机必须非阻塞推进，后续网关任务才能并行运行。 */
        result_led_process(HAL_GetTick());
        osDelay(1U);
    }
}

void MX_FREERTOS_Init(void)
{
    if (osThreadNew(StartDefaultTask, NULL, &s_default_task_attributes) == NULL) {
        Error_Handler();
    }
}

/* 栈溢出表示内存已可能损坏，立即停机，避免继续控制电源或发送错帧。 */
void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    (void)task_name;
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}
