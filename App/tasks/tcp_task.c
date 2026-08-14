/*
 * ===================================================================
 * TCP Server 任务 — 从调度器启动前创建 (freertos.c)
 *
 * 完整实现: LWIP 就绪 → bind 8888 → accept → TLV 帧解析/分发/回传
 *
 * 数据流:
 *   PC → TCP 8888 → tlv_sm 解析 TLV → tlv_dispatch
 *        ├─ 测试命令 → pcToRk_Queue → rk_task → USART1 → RK3588 (先回 ACK)
 *        └─ 本地命令 (reboot/frame_error) → MCU 执行 + ACK
 *   RK3588 响应 → rk_task → rkToPc_Queue → 本任务 → TCP 回 PC
 * ===================================================================
 */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "led.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "state_machine.h"
#include "dispatch.h"
#include "frame.h"
#include <string.h>
#include <errno.h>

#define TCP_PORT    8888
#define RX_BUF_SIZE 1024

extern struct netif gnetif;
extern QueueHandle_t rkToPc_Queue;   /* RK→PC 上报队列 (rk_msg_t) */
extern QueueHandle_t monitorQueue;   /* 监测→TCP 队列 (128B 文本)  */

/* ═══════════════════════════════════════════════════════════════════
 * 诊断跟踪: 放在 .crash_log 节, SWD 可读
 * ═══════════════════════════════════════════════════════════════════ */
__attribute__((section(".crash_log"), used)) volatile uint32_t tcp_diag[8];
#define TCP_DIAG(n, v) do { tcp_diag[n] = (uint32_t)(v); } while(0)

void StartTcpServerTask(void *argument)
{
    (void)argument;
    TCP_DIAG(0, 0xBEEF0001);

    /* 注册内置命令处理器 (幂等) */
    static uint8_t dispatch_inited = 0;
    if (!dispatch_inited) {
        tlv_dispatch_init();
        dispatch_inited = 1;
    }

    /* 阶段1: 等待 LWIP 拿到 IP */
    int retry = 0;
    while (gnetif.ip_addr.addr == 0) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (++retry > 100) {
            TCP_DIAG(1, 0xBEEF00E1);   /* LWIP 超时 */
            vTaskDelete(NULL);
            return;
        }
    }
    TCP_DIAG(1, 0xBEEF0002);

    /* 阶段2: 建监听 socket */
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        TCP_DIAG(2, 0xBEEF00E2);       /* socket 失败 */
        vTaskDelete(NULL);
        return;
    }

    int one = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len         = sizeof(addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(TCP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        TCP_DIAG(2, 0xBEEF00E3);       /* bind 失败 */
        close(listenfd);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listenfd, 1) != 0) {
        TCP_DIAG(2, 0xBEEF00E4);       /* listen 失败 */
        close(listenfd);
        vTaskDelete(NULL);
        return;
    }
    TCP_DIAG(2, 0xBEEF0003);

    /* 阶段3: accept 循环 */
    for (;;) {
        TCP_DIAG(3, 0xBEEF0004);       /* 等待连接 */
        int connfd = accept(listenfd, NULL, NULL);
        if (connfd < 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        TCP_DIAG(3, 0xBEEF0005);       /* 已连接 */
        led_on(LED_CONNECT);

        /* 接收超时 100ms: 让 recv 周期性返回, 以便转发队列 */
        struct timeval rxto;
        rxto.tv_sec  = 0;
        rxto.tv_usec = 100000;
        setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &rxto, sizeof(rxto));

        /* 每个连接独立的状态机 + 接收缓冲 */
        tlv_sm_t sm;
        tlv_sm_init(&sm);
        uint8_t rx[RX_BUF_SIZE];
        uint8_t mon_buf[128];

        /* 阶段4: 服务连接 */
        for (;;) {
            int n = recv(connfd, rx, sizeof(rx), 0);

            if (n > 0) {
                /* 逐字节喂状态机 */
                for (int i = 0; i < n; i++) {
                    int r = tlv_sm_feed(&sm, rx[i]);
                    if (r == 1) {
                        tlv_frame_t frame;
                        tlv_sm_get_frame(&sm, &frame);

                        uint8_t resp[TLV_FRAME_MAX];
                        int rlen = tlv_dispatch(&frame, resp, sizeof(resp));
                        if (rlen > 0) {
                            send(connfd, resp, (size_t)rlen, 0);
                        }
                    }
                }
            } else if (n == 0) {
                /* 对端关闭 */
                TCP_DIAG(4, 0xBEEF00E5);
                break;
            } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
                /* 连接错误 (非超时) */
                TCP_DIAG(4, 0xBEEF00E6);
                break;
            }

            /* 半帧超时复位: 防止截断帧卡死状态机, 丢弃后续正常帧 */
            tlv_sm_check_timeout(&sm, 500);

            /* 转发 RK→PC 上报 (TLV 帧) */
            if (rkToPc_Queue) {
                rk_msg_t msg;
                while (xQueueReceive(rkToPc_Queue, &msg, 0) == pdTRUE) {
                    uint8_t out[TLV_FRAME_MAX];
                    int olen = tlv_pack_cmd(msg.cmd, msg.data, msg.data_len,
                                            msg.sequence, out, sizeof(out));
                    if (olen > 0) {
                        send(connfd, out, (size_t)olen, 0);
                    }
                }
            }

            /* 转发监测报文 (原始文本) */
            if (monitorQueue) {
                while (xQueueReceive(monitorQueue, mon_buf, 0) == pdTRUE) {
                    send(connfd, mon_buf, sizeof(mon_buf), 0);
                }
            }
        }

        close(connfd);
        led_off(LED_CONNECT);
        TCP_DIAG(3, 0xBEEF0006);       /* 已断开 */
    }
}
