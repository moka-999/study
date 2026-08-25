/*
 * ===================================================================
 * PC-MCU TCP Server任务 — 从调度器启动前创建 (freertos.c)
 *
 * 协议边界：本任务只拥有PC TCP连接和PC u32 Seq。不得把RK u64 Seq直接写入
 * PC响应；业务响应必须回显相应PC请求Seq，跨协议关联由后续适配层维护。
 *
 * 正式镜像: LWIP就绪 → bind 8888 → accept → PC固定头组帧 → cJSON严格解析。
 * 当前只校验已确认的START_TEST字段；PC-RK业务转换和正式响应格式尚未冻结，
 * 因此本任务不会编造ACK，也不会把RK TLV或监测文本原样发给PC。
 * PC网络验收镜像: 固定18字节头+cJSON解析后原样回显，仅验证传输框架，
 * 不定义业务响应；由ENABLE_PC_PROTOCOL_NET_TEST隔离。
 *
 * 数据流:
 *   PC → TCP 8888 → pc_protocol_stream → cJSON → 后续PC业务适配层
 *   后续适配层 → 独立RK Seq/TLV → RK3588；响应再映射回原PC Seq
 * ===================================================================
 */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "led.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "pc_protocol.h"
#include "pc_command.h"
#include "pc_test_message.h"
#include <string.h>
#include <errno.h>

#define TCP_PORT    8888
#define RX_BUF_SIZE 1024

/* send()允许短写；任何PC响应都必须循环发送完整PDU。 */
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

/*
 * 业务字段尚未全部冻结。这里只校验已经由PC给出结构的START_TEST，其他命令
 * 先保留严格JSON对象检查，后续在独立业务适配层增加字段合同和响应。
 */
static int pc_validate_business_json(const pc_protocol_frame_t *frame,
                                     const cJSON *root)
{
    if (frame->command_id == PC_COMMAND_CLIENT_HEARTBEAT) {
        /* 心跳合同固定为空对象，不允许借心跳隐式扩展状态字段。 */
        return root->child == NULL ? 0 : -1;
    }
    if (frame->command_id == PC_COMMAND_SERVER_HEARTBEAT) {
        /* 0x0001只允许MCU发给PC，PC反向发送属于方向错误。 */
        return -1;
    }
    if (frame->command_id == PC_COMMAND_START_TEST) {
        pc_test_start_view_t request;
        return pc_test_start_parse(root, &request) == PC_TEST_JSON_OK ? 0 : -1;
    }
    return 0;
}

/* 返回20字节心跳响应：18字节头+空JSON对象，Seq由组帧器原样回显。 */
static int pc_send_heartbeat_response(int socket_fd,
                                      const pc_protocol_frame_t *request)
{
    static const uint8_t empty_json[] = {'{', '}'};
    uint8_t response[PC_PROTOCOL_HEADER_SIZE + sizeof(empty_json)];
    uint32_t response_length = 0U;

    if (pc_protocol_build_response(request,
                                   PC_COMMAND_SERVER_HEARTBEAT,
                                   empty_json,
                                   sizeof(empty_json),
                                   response,
                                   sizeof(response),
                                   &response_length) != 0) {
        return -1;
    }
    return pc_send_all(socket_fd, response, response_length);
}

/* ═══════════════════════════════════════════════════════════════════
 * 诊断跟踪: 放在 .crash_log 节, SWD 可读
 * ═══════════════════════════════════════════════════════════════════ */
__attribute__((section(".crash_log"), used)) volatile uint32_t tcp_diag[8];
#define TCP_DIAG(n, v) do { tcp_diag[n] = (uint32_t)(v); } while(0)

void StartTcpServerTask(void *argument)
{
    (void)argument;
    TCP_DIAG(0, 0xBEEF0001);

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

        /* 接收超时100ms，让任务能检查被截断的半帧是否超时。 */
        struct timeval rxto;
        rxto.tv_sec  = 0;
        rxto.tv_usec = 100000;
        setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &rxto, sizeof(rxto));

        /* 每个TCP连接使用独立PC协议状态，断线后不继承半帧。 */
        pc_protocol_stream_t pc_stream;
        pc_protocol_stream_reset(&pc_stream);
        TickType_t pc_last_byte_tick = xTaskGetTickCount();
        uint8_t rx[RX_BUF_SIZE];

        /* 阶段4: 服务连接 */
        for (;;) {
            int n = recv(connfd, rx, sizeof(rx), 0);

            if (n > 0) {
                int close_for_protocol_error = 0;
                pc_last_byte_tick = xTaskGetTickCount();
                for (int i = 0; i < n; i++) {
                    pc_protocol_feed_result_t result =
                        pc_protocol_stream_push_byte(&pc_stream, rx[i]);
                    if (result == PC_PROTOCOL_FEED_ERROR) {
                        close_for_protocol_error = 1;
                        break;
                    }
                    if (result == PC_PROTOCOL_FEED_FRAME_READY) {
                        pc_protocol_frame_t frame;
                        cJSON *root = NULL;
                        if (pc_protocol_stream_get_frame(&pc_stream, &frame) != 0 ||
                            pc_json_parse_object(frame.json, frame.json_length,
                                                 &root, NULL) != PC_JSON_OK ||
                            pc_validate_business_json(&frame, root) != 0) {
                            cJSON_Delete(root);
                            close_for_protocol_error = 1;
                            break;
                        }
                        cJSON_Delete(root);

                        if (frame.command_id == PC_COMMAND_CLIENT_HEARTBEAT) {
                            if (pc_send_heartbeat_response(connfd, &frame) != 0) {
                                close_for_protocol_error = 1;
                                break;
                            }
                        }
#if defined(ENABLE_PC_PROTOCOL_NET_TEST) && ENABLE_PC_PROTOCOL_NET_TEST
                        /*
                         * 除已冻结心跳外，测试镜像只回显完整PDU：验证Length、Seq、
                         * UTF-8和TCP边界，不把回显伪装成正式业务响应。
                         */
                        if (frame.command_id != PC_COMMAND_CLIENT_HEARTBEAT &&
                            pc_send_all(connfd, pc_stream.buffer,
                                        frame.length) != 0) {
                            close_for_protocol_error = 1;
                            break;
                        }
#else
                        /* 记录最近收到的合法PC命令，便于SWD确认正式入口已经生效。 */
                        TCP_DIAG(5, frame.command_id);
#endif
                        pc_protocol_stream_reset(&pc_stream);
                    }
                }
                if (close_for_protocol_error) {
                    TCP_DIAG(4, 0xBEEF00E7);
                    break;
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

            /* 半帧超时复位: 防止截断帧长期占住单客户端服务器。 */
            if (pc_stream.received != 0U &&
                (xTaskGetTickCount() - pc_last_byte_tick) > pdMS_TO_TICKS(500U)) {
                TCP_DIAG(4, 0xBEEF00E8);
                break;
            }
        }

        close(connfd);
        led_off(LED_CONNECT);
        TCP_DIAG(3, 0xBEEF0006);       /* 已断开 */
    }
}
