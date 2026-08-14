/*
 * ===================================================================
 * 心跳任务 — 独立于 rk_task, 防止被阻塞
 * ===================================================================
 *
 * 职责:
 *   1. 每 1000ms 发送 Type=0x0000 心跳帧 (共享 g_rk_seq_counter)
 *   2. 无其他依赖, 不与 rk_task 的命令收发互相阻塞
 *
 * Seq 体系:
 *   g_rk_seq_counter — MCU→RK3588 统一单调递增 (心跳 + 命令帧)
 * ===================================================================
 */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "uart_router.h"
#include "frame.h"
#include "hb_task.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 * 全局序列号 (与 rk_task 共享, taskENTER_CRITICAL 保护读写)
 * ═══════════════════════════════════════════════════════════════════ */
volatile uint64_t g_rk_seq_counter = 0;

#define HB_INTERVAL_MS  1000

/* ── 简易调试输出 ── */
static void _dbg(const char *s)
{
    uart_send(0, (uint8_t *)s, (uint16_t)strlen(s));
}

/* ═══════════════════════════════════════════════════════════════════
 * 心跳任务
 * ═══════════════════════════════════════════════════════════════════ */
void StartHbTask(void *argument)
{
    uint32_t last_tick = 0;
    (void)argument;

    for (;;) {
        uint32_t now = xTaskGetTickCount();

        if ((now - last_tick) >= pdMS_TO_TICKS(HB_INTERVAL_MS)) {
            last_tick = now;

            /* 原子获取并递增共享序列号 */
            taskENTER_CRITICAL();
            uint64_t seq = g_rk_seq_counter++;
            taskEXIT_CRITICAL();

            uint8_t hb[TLV_FRAME_MAX];
            int len = tlv_pack_heartbeat(seq, hb, sizeof(hb));
            if (len > 0) {
                uart_send(0, hb, (uint16_t)len);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
