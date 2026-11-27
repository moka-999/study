/*
 * ===================================================================
 * B1 PC-MCU TCP Server任务 — 调度前由freertos.c创建
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
 *   pc_test_service → 独立RK Seq/TLV → RK3588；结果按algorithmIndex关联
 * ===================================================================
 */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "app_tasks.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "pc_dispatch.h"
#include "pc_trace.h"
#include "pc_state_machine.h"
#include "task_watchdog.h"
#include "b2_service.h"
#include "handoff_mcu.h"
#include "board_network_config.h"
#include <string.h>
/* 与lwIP socket共用错误码变量，不能读取newlib的另一份errno。 */
#include "lwip/errno.h"

#define RX_BUF_SIZE 1024
#define PC_MCU_TX_SEQUENCE_FIRST 1U

/*
 * send()返回值是“本次实际送入TCP发送缓冲的字节数”，可能小于length，
 * 这不是协议错误。因此从sent偏移继续发送，直到完整PDU提交或socket报错。
 */
static int pc_send_all(int socket_fd, const pc_dispatch_response_t *response, uint32_t sequence)
{
    size_t sent = 0U;
    while (sent < response->length) {
        int result = send(socket_fd, response->data + sent, response->length - sent, 0);
        if (result <= 0) {
            pc_trace_tx(response, sequence, sent, result < 0 ? errno : 0);
            return -1;
        }
        sent += (size_t)result;
    }
    pc_trace_tx(response, sequence, sent, 0);
    pc_dispatch_response_sent(response->command_id);
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

/* listen的创建/关闭始终由唯一TCP任务负责。 */
static int handoff_open_listener(void)
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
    addr.sin_port        = htons(B1_PC_TCP_PORT);
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

void StartTcpServerTask(void *argument)
{
    (void)argument;
    TCP_DIAG(0, 0xBEEF0001);

    /* 启动时等本机IP；超过20秒留下诊断码，地址稍后就绪仍可开始监听。 */
    const TickType_t wait_start = xTaskGetTickCount();
    while (gnetif.ip_addr.addr == 0) {
        if (xTaskGetTickCount() - wait_start >= pdMS_TO_TICKS(20000U)) {
            TCP_DIAG(1, 0xBEEF00E1);
        }
        task_watchdog_progress(TASK_WATCHDOG_TCP);
        vTaskDelay(pdMS_TO_TICKS(200U));
    }
    TCP_DIAG(1, 0xBEEF0002);

    int listenfd = -1;
    TickType_t last_select_error = xTaskGetTickCount() - pdMS_TO_TICKS(1000U);

    /*
     * 阶段3：只在无客户端时监听；accept后关闭监听并释放尚未接手的连接。
     * 已接手的connfd独立存活，退出后才重新监听，避免额外连接占满RX池。
     */
    for (;;) {
        pc_dispatch_poll();
        handoff_mcu_tcp_poll(0, listenfd >= 0);
        if (!handoff_mcu_listen_allowed() && listenfd >= 0) {
            close(listenfd);
            listenfd = -1;
        }
        if (handoff_mcu_listen_allowed() && listenfd < 0) {
            listenfd = handoff_open_listener();
            if (listenfd >= 0) handoff_mcu_tcp_poll(0, 1);
        }
        if (listenfd < 0) {
            b2_service_poll(0);
            task_watchdog_progress(TASK_WATCHDOG_TCP);
            vTaskDelay(pdMS_TO_TICKS(20U));
            continue;
        }
        TCP_DIAG(3, 0xBEEF0004);       /* 等待连接 */
        /* 无连接时也有界等待，避免健康检查把阻塞accept误判为任务卡死。 */
        fd_set readers;
        FD_ZERO(&readers); FD_SET(listenfd, &readers);
        struct timeval accept_wait = {0, 20000};
        int available = select(listenfd + 1, &readers, NULL, NULL, &accept_wait);
        int select_error = available < 0 ? errno : 0;
        task_watchdog_progress(TASK_WATCHDOG_TCP);
        pc_dispatch_poll();
        if (available < 0) {
            /* 每秒最多记录一次。关闭失败且描述符仍有效时保留所有权，避免泄漏。 */
            TickType_t now = xTaskGetTickCount();
            if (now - last_select_error >= pdMS_TO_TICKS(1000U)) {
                last_select_error = now;
                pc_trace_listen_error(select_error);
            }
            if (close(listenfd) == 0 || errno == EBADF) listenfd = -1;
            b2_service_poll(0);
            vTaskDelay(pdMS_TO_TICKS(20U));
            continue;
        }
        if (available == 0) {
            b2_service_poll(0);
            continue;
        }
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        int connfd = accept(listenfd, (struct sockaddr *)&peer, &peer_length);
        if (connfd < 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        /* lwIP关闭监听时清理accept队列及其数据，不关闭已取出的connfd。 */
        if (close(listenfd) != 0) {
            close(connfd);
            vTaskDelay(pdMS_TO_TICKS(20U));
            continue;
        }
        listenfd = -1;
        TCP_DIAG(3, 0xBEEF0005);       /* 已连接 */
        pc_trace_connected((const uint8_t *)&peer.sin_addr.s_addr, ntohs(peer.sin_port));
        const char *close_reason = "protocolError";

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
         * UINT32_MAX后按无符号规则回绕到0，接收端不得把0当成无效值。
         */
        uint32_t mcu_tx_sequence = PC_MCU_TX_SEQUENCE_FIRST;
        TickType_t pc_last_byte_tick = xTaskGetTickCount();
        uint8_t rx[RX_BUF_SIZE];

        /* 阶段4：服务当前连接，任何致命协议错误都跳出并关闭connfd。 */
        for (;;) {
            task_watchdog_progress(TASK_WATCHDOG_TCP);
            pc_dispatch_poll();
            b2_service_poll(1);
            handoff_mcu_tcp_poll(1, 0);
            /* 领取真实RK异步结果，业务层已完成JSON组包；本任务只传输及推进Seq。 */
            pc_dispatch_response_t async_response;
            int ready = pc_dispatch_next_response(mcu_tx_sequence, &async_response);
            if (ready < 0) {
                close_reason = "dispatchError";
                break;
            }
            if (ready > 0) {
                if (pc_send_all(connfd, &async_response, mcu_tx_sequence) != 0) {
                    close_reason = "sendFailed";
                    break;
                }
                ++mcu_tx_sequence;
            }
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

                        pc_trace_rx(&frame);
                        dispatch_result = pc_dispatch_handle(&frame,
                                                             mcu_tx_sequence,
                                                             &response);
                        if (dispatch_result == PC_DISPATCH_ERROR) {
                            close_reason = "dispatchError";
                            close_for_protocol_error = 1;
                            break;
                        }
                        if (dispatch_result == PC_DISPATCH_RESPONSE_READY) {
                            if (pc_send_all(connfd, &response, mcu_tx_sequence) != 0) {
                                close_reason = "sendFailed";
                                close_for_protocol_error = 1;
                                break;
                            }
                            mcu_tx_sequence++;
                        }
                        /*
                         * 0103的accepted仅表示入队；真正算法结论在异步0105上报。
                         * 坏头有界丢弃，完整坏JSON回错后继续；仅恢复超限等致命错误断连。
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
                close_reason = "peerClosed";
                break;
            } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
                /* 负数且不是SO_RCVTIMEO产生的暂时无数据，属于真实socket错误。 */
                pc_trace_rx_error(errno);
                TCP_DIAG(4, 0xBEEF00E6);
                close_reason = "recvFailed";
                break;
            }

            /*
             * 半帧超时直接关闭而非仅reset：截断正文后已无法可靠确认下一字节
             * 是旧正文还是新帧头。关闭可让PC以一个干净的新连接重试。
             */
            if (pc_sm_is_receiving(&pc_sm) &&
                (xTaskGetTickCount() - pc_last_byte_tick) > pdMS_TO_TICKS(500U)) {
                TCP_DIAG(4, 0xBEEF00E8);
                close_reason = "partialTimeout";
                break;
            }
        }

        pc_trace_closed(close_reason);
        pc_dispatch_disconnected();
        close(connfd);
        TCP_DIAG(3, 0xBEEF0006);       /* 已断开 */
    }
}
