/*
 * ===================================================================
 * RK心跳任务 — 独立调度，与rk_task共享发送通道
 * ===================================================================
 *
 * 职责:
 *   1. 每1000ms发送Type=0x0000心跳帧，与命令统一分配RK序号。
 *   2. 独立调度，但仍与rk_task共享UART发送锁，不能称为完全互不阻塞。
 *
 * Seq 体系:
 *   rk_link_next_sequence() — MCU→RK3588序号；与PC Seq无关。
 * ===================================================================
 */
#include "FreeRTOS.h"
#include "task.h"
#include "uart_router.h"
#include "frame.h"
#include "hb_task.h"
#include "rk_link.h"
#include "pwr_ctrl.h"
#include "ota_app.h"

#define HB_INTERVAL_MS  1000

/* ═══════════════════════════════════════════════════════════════════
 * 心跳任务
 * ═══════════════════════════════════════════════════════════════════ */
void StartHbTask(void *argument)
{
    uint32_t last_tick = 0;
    (void)argument;

    for (;;) {
        uint32_t now = xTaskGetTickCount();

        if (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) {
            /* 未上电不主动发送心跳，也不消耗发送Seq。 */
            last_tick = now;
        } else if ((now - last_tick) >= pdMS_TO_TICKS(HB_INTERVAL_MS)) {
            last_tick = now;

            uint64_t seq = rk_link_next_sequence();

            uint8_t hb[TLV_FRAME_MAX];
            int len = tlv_pack_heartbeat(seq, hb, sizeof(hb));
            if (len > 0) {
                /* 心跳不进pending；失败则等下个周期，不能忙循环重试。 */
                (void)uart_send(0, hb, (uint16_t)len);
            }
        }

        ota_app_progress(OTA_TASK_HB);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
