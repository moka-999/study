/*
 * ===================================================================
 * 系统监测任务 — 定时采集 I2C 设备在线状态, 上报 PC
 * ===================================================================
 *
 * 采集周期: 2 秒
 * 上报内容: 软件 I2C(PCF8574) 状态 / 硬件 I2C1 是否有设备 / 风扇转速
 *
 * 数据流: monitor_task → monitorQueue → tcp_task → PC
 *
 * B1 板扩展: 替换占位值为真实 I2C 电压/电流/温度读数
 * ===================================================================
 */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "hal_if.h"
#include <stdio.h>

extern QueueHandle_t monitorQueue;       /* →TCP 的消息队列 (freertos.c 创建) */
extern i2c_if_t      i2c1_if;           /* 硬件 I2C1 接口                */

void StartMonitorTask(void *argument)
{
  char buf[128];
  (void)argument;

  vTaskDelay(pdMS_TO_TICKS(5000));       /* 等 LwIP 初始化完成 */

  for (;;)
  {
    /*
     * ── 采集 ──────────────────────────────────────────────────
     * 1. 硬件 I2C1: 扫描地址 0x28 (24C02 EEPROM 7位地址)
     *    Apollo 板 PB8/PB9 无外接设备 → 返回 NA
     *    B1 板接上 EEPROM/传感器 → 返回 OK
     * 2. 软件 I2C: PCF8574 已通过 myiic 验证在线 → 固定 OK
     * 3. 风扇: Apollo 板无测速 → 固定 0%
     */
    int hw = (i2c1_if.is_ready(&i2c1_if, 0x28) == IF_OK);

    snprintf(buf, sizeof(buf),
      "I2C_SW:OK I2C_HW:%s FAN:RK\r\n",
      hw ? "OK" : "NA");

    /* ── 上报 TCP (队列满则丢弃, 不阻塞) ── */
    /* xQueueSend(monitorQueue, buf, 0); */  /* 暂时关闭, 调试协议 */

    vTaskDelay(pdMS_TO_TICKS(2000));     /* 2 秒一次 */
  }
}
