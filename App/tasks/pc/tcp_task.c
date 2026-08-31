/*
 * ===================================================================
 * B1 PC-MCU TCP Server任务 — 调度前由freertos.c创建
 *
 * 协议边界：本任务只拥有PC TCP连接和PC u32 Seq。不得把RK u64 Seq直接写入
 * PC响应。当前仍为V2旧回显行为，独立发送Seq的新合同尚待实现；
 * 任务移入B1不代表新Seq规则或PC-RK算法业务已经接通。
 *
 * 正式镜像: LWIP就绪 → bind 8888 → accept → PC状态机 → 请求分发层。
 * JSON解析、命令校验和本地响应均由pc_dispatch负责；本任务不理解业务字段，
 * 也不会把RK TLV或监测文本原样发给PC。
 *
 * 数据流:
 *   PC → TCP 8888 → pc_state_machine → pc_dispatch → 后续业务适配层
 *   后续适配层 → 独立RK Seq/TLV → RK3588；业务结果关联合同另行冻结
 * ===================================================================
 */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "app_tasks.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "pc_dispatch.h"
#include "pc_state_machine.h"
#include <string.h>
#include <errno.h>

#define TCP_PORT    8888
#define RX_BUF_SIZE 1024

/*
 * send()返回值是“本次实际送入TCP发送缓冲的字节数”，可能小于length，
 * 这不是协议错误。因此从sent偏移继续发送，直到完整PDU提交或socket报错。
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

    /* 阶段2：建立整个任务生命周期内唯一的监听socket。 */
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        TCP_DIAG(2, 0xBEEF00E2);       /* socket 失败 */
        vTaskDelete(NULL);
        return;
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

    /*
     * 阶段3：单客户端accept循环。listen积压为1，每次只服务当前connfd；
     * 连接退出后才回到accept，因此不会并发修改同一个协议状态或业务上下文。
     */
    for (;;) {
        TCP_DIAG(3, 0xBEEF0004);       /* 等待连接 */
        int connfd = accept(listenfd, NULL, NULL);
        if (connfd < 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        TCP_DIAG(3, 0xBEEF0005);       /* 已连接 */

        /*
         * recv每100ms返回一次EWOULDBLOCK/EAGAIN，使任务有机会执行下面的
         * 500ms半帧超时检查；这不是PC心跳周期，也不会主动断开空闲连接。
         */
        struct timeval rxto;
        rxto.tv_sec  = 0;
        rxto.tv_usec = 100000;
        setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &rxto, sizeof(rxto));

        /*
         * 每个TCP连接使用独立PC协议状态，断线后不继承半帧。rx只承接一次
         * recv的临时数据；pc_sm内部缓冲才保存跨多次recv仍未收齐的PDU。
         */
        pc_sm_t pc_sm;
        pc_sm_init(&pc_sm);
        TickType_t pc_last_byte_tick = xTaskGetTickCount();
        uint8_t rx[RX_BUF_SIZE];

        /* 阶段4：服务当前连接，任何致命协议错误都跳出并关闭connfd。 */
        for (;;) {
            int n = recv(connfd, rx, sizeof(rx), 0);

            if (n > 0) {
                int close_for_protocol_error = 0;
                pc_last_byte_tick = xTaskGetTickCount();
                /*
                 * 逐字节喂流解析器是为了处理任意TCP切分。完成一帧并reset后，
                 * 循环会继续消费rx剩余字节，所以一次recv中的多个粘包不会丢失。
                 */
                for (int i = 0; i < n; i++) {
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
                        dispatch_result = pc_dispatch_handle(&frame, &response);
                        if (dispatch_result == PC_DISPATCH_ERROR ||
                            (dispatch_result == PC_DISPATCH_RESPONSE_READY &&
                             pc_send_all(connfd, response.data,
                                         response.length) != 0)) {
                            close_for_protocol_error = 1;
                            break;
                        }
                        /*
                         * 其他命令目前只验证并记录，不回临时ACK，避免PC工程师把
                         * 尚未冻结的行为当成正式合同；业务适配层接入后在这里投递。
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

        close(connfd);
        TCP_DIAG(3, 0xBEEF0006);       /* 已断开 */
    }
}
