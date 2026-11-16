/* 同IP交接：保留测试任务与电源流程，只有net rk明确准备后才接管RK协议口。 */
#include "handoff_mcu.h"
#include "handoff_rk.h"
#include "handoff_state.h"
#include "serial_net.h"
#include "ip_guard.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "main.h"
#include "uart_router.h"
#include "pwr_ctrl.h"
#include "pc_test_service.h"
#include "pc_power_service.h"
#include "task_watchdog.h"
#include "log_console.h"
#include "rk_link.h"
#include "rk_trace.h"
#include "state_machine.h"
#include <string.h>
#include <stdio.h>

#define HANDOFF_RX_DEPTH 8U
#define HANDOFF_PARSER_IDLE_MS 1000U
#define HANDOFF_TX_WAIT_MS 2500U
#define HANDOFF_TX_POLL_MS 10U
#define HANDOFF_UART_BATCH 8U
#define HANDOFF_LOG_SIZE 128U
/* RK于2026-11-11确认：SPL收到5000[2]=3E00后继续后级引导，无附加参数。 */
#define HANDOFF_RK_CMD_CONTINUE_BOOT 0x003EU
/* 手动交接已独占业务；此令牌只复用正式就绪窗口，不与PC算法并行。 */
#define HANDOFF_SPL_READY_TOKEN 1U

typedef enum {
    HANDOFF_BOOT_PROBE = 0, HANDOFF_BOOT_WAIT_TX, HANDOFF_BOOT_EDK, HANDOFF_BOOT_FAILED
} handoff_boot_phase_t;

typedef struct { uint16_t length; uint8_t data[SN_ETH_MAX]; } handoff_packet_t;
typedef enum { NETWORK_NONE = 0, NETWORK_WITHDRAW, NETWORK_RESTORE, NETWORK_DRAIN } network_operation_t;
static handoff_state_t s_state;
static volatile handoff_command_t s_command;
static volatile uint8_t s_reset_stream, s_active;
static volatile handoff_boot_phase_t s_boot_phase;
static tlv_sm_t s_spl_parser;
static QueueHandle_t s_rx_queue;
static SemaphoreHandle_t s_tx_done;
static struct netif *s_netif;
static uint8_t s_mac[6];
static ip4_addr_t s_ip, s_mask, s_gateway;
static uint8_t s_saved;
static volatile network_operation_t s_network_operation;
static volatile uint8_t s_network_pending, s_network_done, s_network_ok;
static volatile uint8_t s_tx_pending;
static volatile err_t s_tx_result;
static handoff_packet_t s_tx, s_rx;
static sn_server_t s_server;
static sn_parser_t s_parser;
static sn_frame_t s_request, s_response;
static uint8_t s_uart_rx[UART_RX_BUF_SIZE], s_wire[SN_WIRE_MAX];
static uint32_t s_last_byte;
/* 低速应答TC后再切换；首个高速合法请求确认前保留有界回退窗口。 */
static uint32_t s_uart_baud = SN_BOOT_BAUD, s_rate_started;
static uint8_t s_rate_pending, s_rate_trial;
static volatile uint8_t s_baud_unknown;

static void report_baud(const char *event)
{
    char line[96];
    int size = snprintf(line, sizeof(line), "[handoff] UART data baud=%lu %s\r\n",
                        (unsigned long)s_uart_baud, event);
    if (size > 0 && size < (int)sizeof(line)) (void)log_console_write(line, (size_t)size);
}
/* TCP上下文保存最新状态快照；后续状态可覆盖旧快照，不保证逐事件无损日志。 */
static char s_report[HANDOFF_LOG_SIZE];
static size_t s_report_length;

static void report(const char *event)
{
    static const char *const names[] = {
        "mcu", "armed", "withdraw", "rk", "release", "restored", "edkIdle"
    };
    ip4_addr_t address = s_saved ? s_ip : (s_netif ? *netif_ip4_addr(s_netif) : (ip4_addr_t){0});
    int count = snprintf(s_report, sizeof(s_report),
        "[handoff] %s phase=%s ip=%u.%u.%u.%u rkMAC=%02X:%02X:%02X:%02X:%02X:%02X session=%lu\r\n", event,
        names[s_state.phase], (unsigned)ip4_addr1(&address), (unsigned)ip4_addr2(&address),
        (unsigned)ip4_addr3(&address), (unsigned)ip4_addr4(&address),
        (unsigned)s_mac[0], (unsigned)s_mac[1], (unsigned)s_mac[2],
        (unsigned)s_mac[3], (unsigned)s_mac[4], (unsigned)s_mac[5], (unsigned long)s_state.session);
    s_report_length = count > 0 && count < (int)sizeof(s_report) ? (size_t)count : 0U;
}

static void flush_report(void)
{
    if (!s_report_length) return;
    /* 0表示控制回复或TX拥塞，留待下轮；1已入队，-1切换来源或尚未启动则取消。 */
    int result = log_console_write_status(s_report, s_report_length);
    if (result != 0) s_report_length = 0U;
}

int handoff_mcu_prepare(void)
{
    s_rx_queue = xQueueCreate(HANDOFF_RX_DEPTH, sizeof(handoff_packet_t));
    s_tx_done = xSemaphoreCreateBinary();
    return s_rx_queue != NULL && s_tx_done != NULL;
}

void handoff_mcu_attach(struct netif *netif)
{
    memcpy(s_mac, netif->hwaddr, sizeof(s_mac));
    /* MCU为02开头，RK虚拟网卡为同尾部06开头；两者均为本地单播地址。 */
    s_mac[0] = 0x06U;
    ETH->MACA1LR = (uint32_t)s_mac[0] | (uint32_t)s_mac[1] << 8 |
                   (uint32_t)s_mac[2] << 16 | (uint32_t)s_mac[3] << 24;
    ETH->MACA1HR = ETH_MACA1HR_AE | (uint32_t)s_mac[4] | (uint32_t)s_mac[5] << 8;
    ETH->MACPFR |= ETH_MACPFR_PM;
    s_netif = netif;
}

int handoff_mcu_console_request(handoff_command_t command)
{
    /* 调用方已在控制台短临界区内；不做电源、socket或网络操作。 */
    if (s_command != HANDOFF_COMMAND_NONE) return 0;
    s_command = command;
    return 1;
}

int handoff_mcu_busy(void) { return s_state.phase != HANDOFF_NORMAL || s_reset_stream; }
int handoff_mcu_listen_allowed(void) { return handoff_state_listen(&s_state); }
int handoff_mcu_power_allowed(int off_command)
{
    return off_command || (!s_reset_stream && !s_baud_unknown &&
        (s_state.phase == HANDOFF_NORMAL ||
         (s_state.phase == HANDOFF_ARMED && s_boot_phase == HANDOFF_BOOT_PROBE)));
}

/* 回调始终运行在tcpip上下文；撤IP不停止PHY、ETH收发或link线程。 */
static void network_callback(void *unused)
{
    int ok = 0;
    (void)unused;
    if (s_netif && s_network_operation == NETWORK_WITHDRAW &&
        s_state.phase == HANDOFF_WITHDRAWING && handoff_eth_tx_idle() &&
        !ip4_addr_isany_val(*netif_ip4_addr(s_netif))) {
        s_ip = *netif_ip4_addr(s_netif);
        s_mask = *netif_ip4_netmask(s_netif);
        s_gateway = *netif_ip4_gw(s_netif);
        s_saved = 1U;
        ip4_addr_t zero = {0};
        etharp_cleanup_netif(s_netif);
        netif_set_addr(s_netif, &zero, &zero, &zero);
        ok = 1;
    } else if (s_network_operation == NETWORK_DRAIN && !s_tx_pending && handoff_eth_tx_idle()) {
        ok = 1;
    } else if (s_netif && s_network_operation == NETWORK_RESTORE &&
               !s_tx_pending && handoff_eth_tx_idle()) {
        /* 超时或DMA仍持有描述符时保持无IP；不得用等待时长替代排空证据。 */
        if (s_saved) {
            etharp_cleanup_netif(s_netif);
            netif_set_addr(s_netif, &s_ip, &s_mask, &s_gateway);
            if (netif_is_link_up(s_netif)) (void)etharp_gratuitous(s_netif);
            s_saved = 0U;
        }
        ok = 1;
    }
    s_network_ok = (uint8_t)ok;
    s_network_done = 1U;
    s_network_pending = 0U;
}

static void request_network(network_operation_t operation)
{
    if (s_network_pending || s_network_done) return;
    s_network_operation = operation;
    s_network_pending = 1U;
    if (tcpip_try_callback(network_callback, NULL) != ERR_OK) s_network_pending = 0U;
}

void handoff_mcu_tcp_poll(int connected, int listener_open)
{
    static handoff_phase_t last_phase;
    int on = pwr_ctrl_get_state() == PWR_CTRL_STATE_ON;
    int off = pwr_ctrl_get_state() == PWR_CTRL_STATE_OFF;
    int idle = !pc_test_service_busy() && !pc_power_service_busy() && rk_task_handoff_idle();
    handoff_command_t command;
    taskENTER_CRITICAL();
    command = s_command;
    s_command = HANDOFF_COMMAND_NONE;
    taskEXIT_CRITICAL();
    if (command != HANDOFF_COMMAND_NONE) {
        int accepted;
        taskENTER_CRITICAL();
        if (command == HANDOFF_COMMAND_RK) {
            accepted = handoff_state_arm(&s_state, off,
                idle && uart_tx_idle(UART_PORT_RK_PROTOCOL));
            if (accepted) s_reset_stream = 1U;
        } else if (s_state.phase == HANDOFF_EDK_IDLE && off && !pc_power_service_busy()) {
            accepted = 1; /* 地址已归还，下面只检查排空，不人为断开新PC连接。 */
        } else accepted = handoff_state_cancel(&s_state, off && !pc_power_service_busy());
        taskEXIT_CRITICAL();
        report(accepted ? "accepted" : "rejected(OFF/idle)");
    }
    taskENTER_CRITICAL();
    if (!(s_state.phase == HANDOFF_EDK_IDLE && off) && handoff_state_power(&s_state, on, off)) {
        s_active = 0U;
        if (s_state.phase == HANDOFF_NORMAL) s_reset_stream = 1U;
    }
    if (s_state.phase == HANDOFF_RELEASING) s_active = 0U;
    taskEXIT_CRITICAL();

    if (s_network_done) {
        network_operation_t operation = s_network_operation;
        int ok = s_network_ok;
        s_network_done = 0U;
        taskENTER_CRITICAL();
        if (ok && operation == NETWORK_WITHDRAW && s_state.phase == HANDOFF_WITHDRAWING)
            s_state.phase = HANDOFF_OWNED;
        if (ok && operation == NETWORK_RESTORE && s_state.phase == HANDOFF_RELEASING)
            s_state.phase = HANDOFF_RESTORED;
        if (ok && operation == NETWORK_DRAIN && s_state.phase == HANDOFF_EDK_IDLE && off) {
            (void)handoff_state_power(&s_state, 0, 1);
            s_reset_stream = 1U;
        }
        taskEXIT_CRITICAL();
    }
    taskENTER_CRITICAL();
    if (handoff_state_can_withdraw(&s_state, connected, on, idle,
                                   uart_tx_idle(UART_PORT_RK_PROTOCOL)))
        s_state.phase = HANDOFF_WITHDRAWING;
    taskEXIT_CRITICAL();
    if (s_state.phase == HANDOFF_EDK_IDLE && off && !s_network_pending)
        request_network(NETWORK_DRAIN);
    /* 必须先由TCP任务关闭监听；授权后的RELEASE同样等原链路排空才恢复。 */
    if (!connected && !listener_open && !s_network_pending) {
        if (s_state.phase == HANDOFF_WITHDRAWING) request_network(NETWORK_WITHDRAW);
        if (s_state.phase == HANDOFF_RELEASING) request_network(NETWORK_RESTORE);
    }
    if (listener_open && s_state.phase == HANDOFF_RESTORED) {
        taskENTER_CRITICAL();
        handoff_state_listener_ready(&s_state);
        if (s_state.phase == HANDOFF_NORMAL) s_reset_stream = 1U;
        taskEXIT_CRITICAL();
    }
    if (last_phase != s_state.phase) {
        last_phase = s_state.phase;
        report("state");
    }
    flush_report();
}

int handoff_mcu_input(struct pbuf *p)
{
    uint8_t header[14];
    static const uint8_t broadcast[6] = {255,255,255,255,255,255};
    if (!s_netif || p->tot_len < sizeof(header) ||
        pbuf_copy_partial(p, header, sizeof(header), 0) != sizeof(header)) return 0;
    int ours = memcmp(header, s_mac, 6U) == 0;
    int bcast = memcmp(header, broadcast, 6U) == 0;
    int mcast = header[0] == 1U && header[1] == 0U && header[2] == 0x5EU && header[3] < 0x80U;
    if (!ours && !bcast && !mcast) return 0;
    if (!s_active || pwr_ctrl_get_state() != PWR_CTRL_STATE_ON || p->tot_len > SN_ETH_MAX) return ours;
    s_rx.length = p->tot_len;
    if (pbuf_copy_partial(p, s_rx.data, p->tot_len, 0) != p->tot_len ||
        !sn_ip_guard_rx(s_rx.data, s_rx.length, s_mac, (const uint8_t *)&s_ip.addr)) return ours;
    taskENTER_CRITICAL();
    if (s_active) (void)xQueueSend(s_rx_queue, &s_rx, 0U);
    taskEXIT_CRITICAL();
    return ours;
}

/* 仅tcpip线程触碰linkoutput；关闭bridge后，已经排队但尚未发出的包直接丢弃。 */
static void transmit_callback(void *unused)
{
    struct pbuf *p = NULL;
    err_t result = ERR_IF;
    (void)unused;
    if (s_active && s_netif && netif_is_link_up(s_netif) &&
        pwr_ctrl_get_state() == PWR_CTRL_STATE_ON) {
        p = pbuf_alloc(PBUF_RAW, s_tx.length, PBUF_RAM);
        result = p ? pbuf_take(p, s_tx.data, s_tx.length) : ERR_MEM;
        if (result == ERR_OK) result = s_netif->linkoutput(s_netif, p);
        if (p) pbuf_free(p);
    }
    taskENTER_CRITICAL();
    s_tx_result = result;
    s_tx_pending = 0U;
    (void)xSemaphoreGive(s_tx_done);
    taskEXIT_CRITICAL();
}

static uint8_t transmit(const sn_frame_t *q)
{
    if (!s_active || !s_netif || !netif_is_link_up(s_netif) || s_tx_pending) return SN_NOT_READY;
    if (!sn_ip_guard_tx(q->data, q->length, s_mac, (const uint8_t *)&s_ip.addr)) return SN_INVALID;
    (void)xSemaphoreTake(s_tx_done, 0U);
    memcpy(s_tx.data, q->data, q->length);
    s_tx.length = q->length;
    s_tx_pending = 1U;
    if (tcpip_try_callback(transmit_callback, NULL) != ERR_OK) {
        s_tx_pending = 0U;
        return SN_IO_ERROR;
    }
    /* 有界等待期间仍推进RK健康标志；超时保留callback对缓冲的所有权。 */
    for (unsigned elapsed = 0U; elapsed < HANDOFF_TX_WAIT_MS; elapsed += HANDOFF_TX_POLL_MS) {
        if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(HANDOFF_TX_POLL_MS)) == pdTRUE)
            return s_tx_result == ERR_OK ? SN_OK : SN_IO_ERROR;
        task_watchdog_progress(TASK_WATCHDOG_RK);
    }
    return SN_IO_ERROR;
}

static void execute(void *unused, const sn_frame_t *q, sn_frame_t *r)
{
    static handoff_packet_t packet;
    (void)unused;
    r->status = SN_INVALID;
    if (q->op == SN_HELLO && q->length == SN_RATE_REQUEST_SIZE) {
        uint32_t baud = (uint32_t)q->data[0] | (uint32_t)q->data[1] << 8 |
                        (uint32_t)q->data[2] << 16 | (uint32_t)q->data[3] << 24;
        if (baud != SN_DATA_BAUD || s_uart_baud != SN_BOOT_BAUD ||
            !s_state.authorized || s_state.phase != HANDOFF_OWNED ||
            s_state.session != q->session) return;
        memcpy(r->data, q->data, SN_RATE_REQUEST_SIZE);
        r->length = SN_RATE_REQUEST_SIZE;
        r->status = SN_OK;
        return;
    }
    if (q->op != SN_SEND && q->length) return;
    if (q->op == SN_HELLO) {
        taskENTER_CRITICAL();
        r->status = handoff_state_hello(&s_state, q->session);
        if (r->status == SN_OK) s_active = 1U;
        taskEXIT_CRITICAL();
        if (r->status != SN_OK) return;
        memcpy(r->data + SN_HELLO_MAC_OFFSET, s_mac, sizeof(s_mac));
        r->data[SN_HELLO_LINK_OFFSET] = netif_is_link_up(s_netif) ? 1U : 0U;
        r->data[SN_HELLO_MTU_OFFSET] = (uint8_t)SN_MTU;
        r->data[SN_HELLO_MTU_OFFSET + 1U] = (uint8_t)(SN_MTU >> 8);
        memcpy(r->data + SN_HELLO_IP_OFFSET, &s_ip.addr, 4U);
        memcpy(r->data + SN_HELLO_MASK_OFFSET, &s_mask.addr, 4U);
        memcpy(r->data + SN_HELLO_GATEWAY_OFFSET, &s_gateway.addr, 4U);
        r->length = SN_HELLO_INFO_SIZE;
        return;
    }
    if (q->op == SN_RELEASE) {
        taskENTER_CRITICAL();
        r->status = handoff_state_release(&s_state, q->session);
        if (r->status != SN_INVALID) s_active = 0U;
        taskEXIT_CRITICAL();
        if (r->status != SN_INVALID) xQueueReset(s_rx_queue);
        return;
    }
    if (!s_state.authorized || s_state.session != q->session || s_state.phase != HANDOFF_OWNED) {
        r->status = SN_NOT_READY;
        return;
    }
    if (q->op == SN_RESET || q->op == SN_STOP) {
        s_active = 0U;
        xQueueReset(s_rx_queue);
        s_active = q->op == SN_RESET;
        r->status = SN_OK;
    } else if (q->op == SN_SEND) r->status = transmit(q);
    else if (q->op == SN_POLL) {
        r->status = s_active && netif_is_link_up(s_netif) ? SN_EMPTY : SN_NOT_READY;
        if (r->status == SN_EMPTY && xQueueReceive(s_rx_queue, &packet, 0U) == pdTRUE) {
            r->length = packet.length;
            memcpy(r->data, packet.data, packet.length);
            r->status = SN_OK;
        }
    }
}

/* 仅在本轮SPL心跳确认、业务与UART空闲后发一次；入队失败也不自动重发。 */
static void continue_spl_boot(void)
{
    if (s_state.phase != HANDOFF_ARMED || s_boot_phase != HANDOFF_BOOT_PROBE ||
        pwr_ctrl_get_state() != PWR_CTRL_STATE_ON || pc_power_service_busy() ||
        pc_test_service_busy() || !rk_task_handoff_idle() ||
        rk_link_ready_poll(HANDOFF_SPL_READY_TOKEN) != 1 ||
        !uart_tx_idle(UART_PORT_RK_PROTOCOL)) return;
    uint8_t wire[TLV_CMD_FRAME_SIZE];
    s_boot_phase = HANDOFF_BOOT_WAIT_TX;
    int length = tlv_pack_cmd(HANDOFF_RK_CMD_CONTINUE_BOOT, NULL, 0U,
                             rk_link_next_sequence(), wire, sizeof(wire));
    int queued = length == TLV_CMD_FRAME_SIZE &&
        uart_send(UART_PORT_RK_PROTOCOL, wire, (uint16_t)length) == 0;
    rk_trace_sent(HANDOFF_RK_CMD_CONTINUE_BOOT, wire, length, queued);
    if (!queued) s_boot_phase = HANDOFF_BOOT_FAILED;
}

int handoff_mcu_rk_poll(void)
{
    if (s_reset_stream) {
        /* 确定OFF后的新一轮必须先恢复115200，再允许读取SPL协议。 */
        if (s_uart_baud != SN_BOOT_BAUD || s_baud_unknown) {
            if (!uart_tx_idle(UART_PORT_RK_PROTOCOL) ||
                uart_set_baud(UART_PORT_RK_PROTOCOL, SN_BOOT_BAUD) != IF_OK) return 1;
            s_uart_baud = SN_BOOT_BAUD;
            s_baud_unknown = 0U;
        }
        s_rate_pending = s_rate_trial = 0U;
        /* server/parser仅RK任务拥有，TCP只发复位标记，不并发改缓存。 */
        memset(&s_server, 0, sizeof(s_server));
        sn_parser_reset(&s_parser);
        tlv_sm_reset(&s_spl_parser);
        s_boot_phase = HANDOFF_BOOT_PROBE;
        if (s_state.phase == HANDOFF_ARMED)
            (void)rk_link_ready_begin(HANDOFF_SPL_READY_TOKEN);
        xQueueReset(s_rx_queue);
        s_reset_stream = 0U;
    }
    if (!handoff_mcu_busy()) return 0;
    if (s_rate_pending) {
        if (!uart_tx_idle(UART_PORT_RK_PROTOCOL)) return 1;
        s_rate_pending = 0U;
        if (uart_set_baud(UART_PORT_RK_PROTOCOL, SN_DATA_BAUD) != IF_OK) {
            s_baud_unknown = 1U; /* HAL或RX重启失败时不能假设硬件还在旧速率。 */
            s_boot_phase = HANDOFF_BOOT_FAILED;
            report_baud("switch failed; power cycle RK to retry");
            return 1;
        }
        s_uart_baud = SN_DATA_BAUD;
        s_rate_started = HAL_GetTick();
        s_rate_trial = 1U;
        sn_parser_reset(&s_parser);
        report_baud("trial");
    }
    if (s_rate_trial && (uint32_t)(HAL_GetTick() - s_rate_started) >= SN_RATE_TRIAL_MS &&
        uart_tx_idle(UART_PORT_RK_PROTOCOL)) {
        if (uart_set_baud(UART_PORT_RK_PROTOCOL, SN_BOOT_BAUD) != IF_OK) {
            s_baud_unknown = 1U;
            s_boot_phase = HANDOFF_BOOT_FAILED;
            report_baud("fallback failed; power cycle RK to retry");
            return 1;
        }
        s_uart_baud = SN_BOOT_BAUD;
        s_rate_trial = 0U;
        sn_parser_reset(&s_parser);
        report_baud("fallback: no fast HELLO");
        /* 保留低速速率请求缓存；ACK丢失后同序号重试可重新协商。 */
    }
    if (s_boot_phase == HANDOFF_BOOT_FAILED) return 1;
    if (s_boot_phase == HANDOFF_BOOT_WAIT_TX) {
        /* 先证明最后停止位已经发出；不等待尚未约定的RK ACK，不据此宣布EDK就绪。 */
        if (!uart_tx_idle(UART_PORT_RK_PROTOCOL)) return 1;
        s_boot_phase = HANDOFF_BOOT_EDK;
        sn_parser_reset(&s_parser);
        static const char sent[] = "[handoff] SPL cmd=003E tx=complete; waiting EDK HELLO\r\n";
        (void)log_console_write(sent, sizeof(sent) - 1U);
    }
    uint32_t now = HAL_GetTick();
    if ((uint32_t)(now - s_last_byte) > HANDOFF_PARSER_IDLE_MS) sn_parser_reset(&s_parser);
    for (unsigned block = 0U; block < HANDOFF_UART_BATCH; ++block) {
        int count = uart_poll_rx(UART_PORT_RK_PROTOCOL, s_uart_rx, sizeof(s_uart_rx));
        if (count == 0) break;
        if (count < 0) {
            sn_parser_reset(&s_parser);
            tlv_sm_reset(&s_spl_parser);
            continue;
        }
        s_last_byte = now;
        for (int i = 0; i < count; ++i) {
            if (s_state.phase == HANDOFF_ARMED && s_boot_phase == HANDOFF_BOOT_PROBE &&
                tlv_sm_feed(&s_spl_parser, s_uart_rx[i]) == 1) {
                tlv_frame_t frame;
                if (tlv_sm_get_frame(&s_spl_parser, &frame) >= 0) {
                    rk_task_handoff_receive(&frame);
                    sn_parser_reset(&s_parser);
                    continue;
                }
            }
            if (sn_feed(&s_parser, s_uart_rx[i], &s_request) != 1) continue;
            if (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) continue;
            if (s_boot_phase == HANDOFF_BOOT_PROBE) {
                /* 自动引导前级直接发合法HELLO；识别后停TLV，也不补发003E。 */
                if (s_state.phase != HANDOFF_ARMED || s_request.op != SN_HELLO ||
                    s_request.status != 0U || s_request.length != 0U || !s_request.session) continue;
                s_boot_phase = HANDOFF_BOOT_EDK;
                tlv_sm_reset(&s_spl_parser);
            }
            int handled = sn_server_handle(&s_server, &s_request, &s_response, execute, NULL);
            if (s_rate_trial && handled >= 0 && s_response.status == SN_OK &&
                s_request.op == SN_HELLO && s_request.length == 0U) {
                s_rate_trial = 0U;
                report_baud("confirmed");
            }
            uint16_t size = sn_pack(&s_response, s_wire, sizeof(s_wire));
            int queued = size && uart_send(UART_PORT_RK_PROTOCOL, s_wire, size) == IF_OK;
            if (queued && s_uart_baud == SN_BOOT_BAUD && handled >= 0 &&
                s_request.op == SN_HELLO && s_request.length == SN_RATE_REQUEST_SIZE &&
                s_response.status == SN_OK && s_response.length == SN_RATE_REQUEST_SIZE) {
                s_rate_pending = 1U;
                return 1;
            }
        }
    }
    (void)tlv_sm_check_timeout(&s_spl_parser, RK_BYTE_TIMEOUT_MS);
    continue_spl_boot();
    if (s_state.phase == HANDOFF_ARMED && s_boot_phase == HANDOFF_BOOT_PROBE)
        rk_task_handoff_heartbeat();
    return 1;
}
