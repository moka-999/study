/*
 * 阿波罗 V2 电源时序波形测试任务
 *
 * 用途：在没有 B1 真板和 TPS62868 实物的阶段，让逻辑分析仪直接观察
 * pwr_ctrl 的完整上电与反向下电顺序。该任务只存在于
 * ENABLE_PWR_WAVE_TEST 开发镜像，正式构建不会创建它。
 *
 * 阿波罗V2实脚验证接点（与B1目标引脚完全一致）：
 *   P2-31 / PC6  = 4.0V EN
 *   P2-32 / PC7  = 3.3V EN
 *   P3-31 / PG11 = DDR EN
 *
 * P2/P3是开发板上的两组2x22扩展排针；测试时不得插摄像头或LCD，
 * 避免DCMI_D0/DCMI_D1/LCD_B3复用负载影响波形。
 *
 * 500ms 仅为便于人眼和低采样率逻辑分析仪识别的测试间隔，不是 B1
 * 获批电源参数，也不得复制到量产配置。
 */
#include "FreeRTOS.h"
#include "task.h"

#include "main.h"
#include "pwr_ctrl.h"

#if defined(ENABLE_PWR_WAVE_TEST) && ENABLE_PWR_WAVE_TEST

#define PWR_WAVE_STEP_MS       500U
#define PWR_WAVE_ON_HOLD_MS   1000U
#define PWR_WAVE_OFF_HOLD_MS  1000U

static const pwr_ctrl_config_t s_wave_config = {
    /* 测试镜像暂用 4.0V 在前，仅用于生成容易识别的固定波形。 */
    .rail_order = PWR_CTRL_RAIL_ORDER_4V0_FIRST,
    .first_to_second_delay_ms = PWR_WAVE_STEP_MS,
    .second_to_ddr_delay_ms = PWR_WAVE_STEP_MS,
    .ddr_enable_to_config_delay_ms = PWR_WAVE_STEP_MS,
    .ddr_to_system_off_delay_ms = PWR_WAVE_STEP_MS,
    .system_rail_off_spacing_ms = PWR_WAVE_STEP_MS,
};

void StartPwrWaveTask(void *argument)
{
    (void)argument;

    /* 等系统启动稳定后再输出首个边沿，便于逻辑分析仪设置触发。 */
    vTaskDelay(pdMS_TO_TICKS(2000U));

    if (pwr_ctrl_configure(&s_wave_config) != PWR_CTRL_OK)
        vTaskDelete(NULL);

    for (;;) {
        if (pwr_ctrl_request_on(HAL_GetTick()) != PWR_CTRL_OK)
            vTaskDelete(NULL);

        /* defaultTask 周期推进状态机；本任务只等待状态，避免并发推进。 */
        while (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON)
            vTaskDelay(pdMS_TO_TICKS(10U));

        vTaskDelay(pdMS_TO_TICKS(PWR_WAVE_ON_HOLD_MS));
        if (pwr_ctrl_request_off(HAL_GetTick()) != PWR_CTRL_OK)
            vTaskDelete(NULL);

        while (pwr_ctrl_get_state() != PWR_CTRL_STATE_OFF)
            vTaskDelay(pdMS_TO_TICKS(10U));

        vTaskDelay(pdMS_TO_TICKS(PWR_WAVE_OFF_HOLD_MS));
    }
}

#endif
