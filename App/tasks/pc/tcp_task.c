/*
 * ===================================================================
 * Apollo V2 PC-MCU TCP Server任务 — 调度前由freertos.c创建
 *
 * 协议边界：本任务拥有PC TCP连接和MCU发送u32 Seq。不得把PC请求Seq或
 * RK u64 Seq写入MCU发送头；业务结果关联合同仍与Seq解耦。
 *
 * 正式镜像: LWIP就绪 → bind 8888 → accept → PC状态机 → 请求分发层。
 * JSON解析、命令校验和本地响应均由pc_dispatch负责；本任务不理解业务字段，
 * 也不会把RK TLV或监测文本原样发给PC。
 *
 * 数据流:
 *   PC → TCP 8888 → pc_state_machine → pc_dispatch → pc_test_service
 *   pc_test_service → 独立RK Seq/TLV → RK3588；最终结果按algorithmIndex关联
 * ===================================================================
 */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "led.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "pc_dispatch.h"
#include "pc_state_machine.h"
#include "task_watchdog.h"
#include "b2_service.h"
#include <string.h>
/* 与lwIP socket共用错误码变量，不能读取newlib的另一份errno。 */
#include "lwip/errno.h"

#define TCP_PORT    8888
#define RX_BUF_SIZE 1024
#define PC_MCU_TX_SEQUENCE_FIRST 1U

/**
 * @brief 循环处理短发送，直到完整PDU交给TCP发送缓冲。
 * @param[in] socket_fd 已连接socket。
 * @param[in] data 至少length字节，调用期间有效；非零长度要求非NULL。
 * @param[in] length 待发送字节数，0直接成功。
 * @return 0全部提交；-1任一次send返回非正值，可能已提交部分前缀。
 * @note 不检查指针或重试EINTR/EAGAIN；失败后调用方关闭连接，避免重发整帧
 *       造成前缀重复。成功不等于PC应用已收到或处理。
 */
static int pc_send_all(int socket_fd, const uint8_t *data, size_t length)
{
    size_t sent = 0U;
    while (sent < length) {
        int result = send(socket_fd, data + sent, length - sent, 0);
        if (result <= 0) return -1;
        sent += (size_t)result;
    }
    return 0;
}

extern struct netif gnetif;

/* ═══════════════════════════════════════════════════════════════════
 * 诊断跟踪：放在NOLOAD的.crash_log节，异常/复位后仍可通过SWD读取。
 * [0]任务启动；[1]LWIP/IP；[2]监听socket；[3]连接生命周期；
 * [4]断线原因；[5]正式镜像最近收到的合法CommandID；[6..7]预留。
 * 数值使用0xBEEF前缀，便于和未初始化内存、普通业务数据区分。
 * ═══════════════════════════════════════════════════════════════════ */
__attribute__((section(".crash_log"), used)) volatile uint32_t tcp_diag[8];
#define TCP_DIAG(n, v) do { tcp_diag[n] = (uint32_t)(v); } while(0)

/* 仅无客户端时创建监听；当前连接退出后重新调用。 */
static int pc_open_listener(void)
{
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        TCP_DIAG(2, 0xBEEF00E2);       /* socket 失败 */
        return -1;
    }

    /* 允许固件重启后快速重新bind 8888，减少旧TCP连接TIME_WAIT的影响。 */
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
        return -1;
    }
    if (listen(listenfd, 1) != 0) {
        TCP_DIAG(2, 0xBEEF00E4);       /* listen 失败 */
        close(listenfd);
        return -1;
    }
    TCP_DIAG(2, 0xBEEF0003);

    return listenfd;
}

/**
 * @brief FreeRTOS单客户端TCP服务入口，拥有连接及MCU发送Seq。
 * @param[in] argument 未使用，可为NULL。
 * @note 等待lwIP/IP后监听8888，逐字节解析并同步分发；监听创建失败有界等待后重试。
 *       帧头垃圾有界丢弃，完整坏JSON回错后继续；恢复超限、半帧超时或
 *       传输/组包失败关闭当前连接，再等待下一客户端。
 *       JSON/硬件业务由pc_dispatch负责，本任务不透传RK原始帧。
 */
void StartTcpServerTask(void *argument)
{
    (void)argument;
    TCP_DIAG(0, 0xBEEF0001);

    /*
     * 阶段1：等待defaultTask完成MX_LWIP_Init并写入本机IP。
     * 最长约20秒（100次*200ms），失败后删除本任务，避免无IP时反复建socket。
     */
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

    int listenfd = -1;

    /*
     * 阶段3：accept后关闭监听并清理额外连接；当前客户端退出后恢复监听。
     * 业务始终只处理一个connfd，额外连接不能长期占用RX缓冲。
     */
    for (;;) {
        if (listenfd < 0) {
            listenfd = pc_open_listener();
            if (listenfd < 0) {
                pc_dispatch_poll();
                b2_service_poll(0);
                task_watchdog_progress(TASK_WATCHDOG_TCP);
                vTaskDelay(pdMS_TO_TICKS(500U));
                continue;
            }
        }
        TCP_DIAG(3, 0xBEEF0004);       /* 等待连接 */
        /* 无PC连接也是正常状态；有界select让任务自己证明仍在工作。 */
        fd_set readers;
        FD_ZERO(&readers); FD_SET(listenfd, &readers);
        struct timeval accept_wait = {0, 20000};
        int available = select(listenfd + 1, &readers, NULL, NULL, &accept_wait);
        task_watchdog_progress(TASK_WATCHDOG_TCP);
        pc_dispatch_poll();
        if (available < 0) { close(listenfd); vTaskDelete(NULL); return; }
        if (available == 0) {
            b2_service_poll(0);
            continue;
        }
        int connfd = accept(listenfd, NULL, NULL);
        if (connfd < 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        /* lwIP清理尚未accept的连接及数据，已取出的connfd继续服务。 */
        if (close(listenfd) != 0) {
            close(connfd);
            vTaskDelay(pdMS_TO_TICKS(20U));
            continue;
        }
        listenfd = -1;
        TCP_DIAG(3, 0xBEEF0005);       /* 已连接 */
        led_on(LED_CONNECT);

        /*
         * recv每20ms返回一次EWOULDBLOCK/EAGAIN，使任务有机会推进B2状态及下面的
         * 500ms半帧超时检查；这不是PC心跳周期，也不会主动断开空闲连接。
         */
        struct timeval rxto;
        rxto.tv_sec  = 0;
        rxto.tv_usec = 20000;
        setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &rxto, sizeof(rxto));
        struct timeval txto = {1, 0};
        setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO, &txto, sizeof(txto));

        /*
         * 每个TCP连接使用独立PC协议状态，断线后不继承半帧。rx只承接一次
         * recv的临时数据；pc_sm内部缓冲才保存跨多次recv仍未收齐的PDU。
         */
        pc_sm_t pc_sm;
        pc_sm_init(&pc_sm);
        /*
         * 每条连接独立从1开始；只有完整PDU成功交给TCP后才推进。u32在
         * UINT32_MAX后按无符号规则回绕到0。首值/重连/回绕仍是暂定实现，
         * 最终合同须由PC双方确认，不能把本段注释当作批准值。
         */
        uint32_t mcu_tx_sequence = PC_MCU_TX_SEQUENCE_FIRST;
        TickType_t pc_last_byte_tick = xTaskGetTickCount();
        uint8_t rx[RX_BUF_SIZE];

        /* 阶段4：服务当前连接，任何致命协议错误都跳出并关闭connfd。 */
        for (;;) {
            task_watchdog_progress(TASK_WATCHDOG_TCP);
            pc_dispatch_poll();
            b2_service_poll(1);
            /* 正式异步结果由业务层组包，TCP只负责传输及本端Seq。 */
            pc_dispatch_response_t async_response;
            int ready = pc_dispatch_next_response(mcu_tx_sequence, &async_response);
            if (ready < 0 || (ready > 0 && pc_send_all(connfd, async_response.data,
                                                    async_response.length) != 0)) break;
            if (ready > 0) ++mcu_tx_sequence;
            int n = recv(connfd, rx, sizeof(rx), 0);

            if (n > 0) {
                int close_for_protocol_error = 0;
                pc_last_byte_tick = xTaskGetTickCount();

                /*
                 * 逐字节喂流解析器是为了处理任意TCP切分。完成一帧并reset后，
                 * 循环会继续消费rx剩余字节，所以一次recv中的多个粘包不会丢失。
                 */
                for (int i = 0; i < n; i++) {
                    /*
                     * 本字节已经由recv交付，长业务处理后的MCU耗时不能冒充
                     * PC半帧停顿。每次实际消费字节都刷新计时，尤其覆盖同一
                     * recv内“完整控制帧+下一帧前缀”的粘包场景。
                     */
                    pc_last_byte_tick = xTaskGetTickCount();
                    pc_sm_feed_result_t result = pc_sm_feed(&pc_sm, rx[i]);
                    if (result == PC_SM_FEED_ERROR) {
                        close_for_protocol_error = 1;
                        break;
                    }
                    if (result == PC_SM_FEED_FRAME_READY) {
                        pc_protocol_frame_t frame;
                        pc_dispatch_response_t response;
                        pc_dispatch_result_t dispatch_result;
                        /*
                         * 状态机只负责组帧；JSON语法、命令方向、业务字段以及
                         * 本地同步响应统一交给dispatch，TCP任务不再理解这些细节。
                         */
                        if (pc_sm_get_frame(&pc_sm, &frame) != 0) {
                            close_for_protocol_error = 1;
                            break;
                        }
                        dispatch_result = pc_dispatch_handle(&frame,
                                                             mcu_tx_sequence,
                                                             &response);
                        if (dispatch_result == PC_DISPATCH_ERROR) {
                            close_for_protocol_error = 1;
                            break;
                        }
                        if (dispatch_result == PC_DISPATCH_RESPONSE_READY) {
                            if (pc_send_all(connfd, response.data,
                                            response.length) != 0) {
                                close_for_protocol_error = 1;
                                break;
                            }
                            mcu_tx_sequence++;
                        }
                        /*
                         * 0103的accepted只表示请求已入队；真实测试结论由后续
                         * 异步0105给出，不能把受理应答当作算法PASS。
                         */
                        TCP_DIAG(5, frame.command_id);
                        /* frame.json借用状态机缓冲，必须在处理完成后再reset。 */
                        pc_sm_reset(&pc_sm);
                    }
                }
                if (close_for_protocol_error) {
                    TCP_DIAG(4, 0xBEEF00E7);
                    break;
                }
            } else if (n == 0) {
                /* TCP有序关闭：PC调用close/shutdown，当前连接生命周期结束。 */
                TCP_DIAG(4, 0xBEEF00E5);
                break;
            } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
                /* 负数且不是SO_RCVTIMEO产生的暂时无数据，属于真实socket错误。 */
                TCP_DIAG(4, 0xBEEF00E6);
                break;
            }

            /*
             * 半帧超时直接关闭而非仅reset：截断正文后已无法可靠确认下一字节
             * 是旧正文还是新帧头。关闭可让PC以一个干净的新连接重试。
             */
            if (pc_sm_is_receiving(&pc_sm) &&
                (xTaskGetTickCount() - pc_last_byte_tick) > pdMS_TO_TICKS(500U)) {
                TCP_DIAG(4, 0xBEEF00E8);
                break;
            }
        }

        pc_dispatch_disconnected();
        close(connfd);
        led_off(LED_CONNECT);
        TCP_DIAG(3, 0xBEEF0006);       /* 已断开 */
    }
}
