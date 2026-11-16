/* 同IP串口网卡：正式SLT或net rk预约后按阶段接管RK协议口，电源仍走正式流程。 */
#include "handoff_mcu.h"
#include "handoff_rk.h"
#include "handoff_state.h"
#include "serial_net.h"
#include "ip_guard.h"
#include "net_mux.h"
#include "board_network_config.h"
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
#define HANDOFF_IDLE_WAIT_MS 1U
#define HANDOFF_LOG_SIZE 128U
#define HANDOFF_START_TIMEOUT_MS 60000U
/* RK于2026-11-11确认：SPL收到5000[2]=3E00后继续后级引导，无附加参数。 */
#define HANDOFF_RK_CMD_CONTINUE_BOOT 0x003EU
/* 手动交接已独占业务；此令牌只复用正式就绪窗口，不与PC算法并行。 */
#define HANDOFF_SPL_READY_TOKEN 1U

/* 接收缓冲必须在一帧内不触发TC；HAL NORMAL DMA满块重启期间对端不会暂停发送。 */
_Static_assert(UART_RX_BUF_SIZE > SN_WIRE_MAX, "UART RX must hold a complete SerialNet wire frame");

typedef enum {
    HANDOFF_BOOT_PROBE = 0, HANDOFF_BOOT_WAIT_TX, HANDOFF_BOOT_EDK, HANDOFF_BOOT_FAILED
} handoff_boot_phase_t;

typedef struct { uint16_t length; uint8_t data[SN_ETH_MAX]; } handoff_packet_t;
typedef enum { NETWORK_NONE = 0, NETWORK_WITHDRAW, NETWORK_RESTORE, NETWORK_DRAIN } network_operation_t;
static handoff_state_t s_state;
static volatile handoff_command_t s_command;
static volatile uint8_t s_reset_stream, s_active;
static volatile uint8_t s_stopping;
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
static uint32_t s_uart_baud = SN_BOOT_BAUD, s_rate_started, s_requested_baud;
static uint8_t s_rate_pending, s_rate_trial;
static volatile uint8_t s_baud_unknown;
/* 自动SLT的业务归属由TCP上下文设置；RK只推进串口状态和最终恢复标记。
 * ON时预约先等本轮真实OFF，不能把旧运行电源记成新会话已启动。 */
static volatile struct {
    uint32_t cookie, prepared_ms;
    uint8_t waiting_off, prepared, started, finished, release_queued;
} s_slt;

static int slt_reserved(void) { return s_slt.cookie != 0U && !s_slt.finished; }
static int slt_start_expired(void)
{
    return slt_reserved() && s_slt.prepared && !s_state.authorized &&
        (uint32_t)(HAL_GetTick() - s_slt.prepared_ms) >= HANDOFF_START_TIMEOUT_MS;
}

static void slt_arm_off(void)
{
    (void)handoff_state_arm(&s_state, 1, 1);
    s_slt.waiting_off = 0U;
    s_reset_stream = 1U;
}

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
    static const char *const shared_names[] = {
        "mcu", "armed", "prepare", "shared", "quiesce", "ready", "edkIdle"
    };
    ip4_addr_t address = s_saved ? s_ip : (s_netif ? *netif_ip4_addr(s_netif) : (ip4_addr_t){0});
    int count = snprintf(s_report, sizeof(s_report),
        "[handoff] %s phase=%s ip=%u.%u.%u.%u rkMAC=%02X:%02X:%02X:%02X:%02X:%02X session=%lu\r\n", event,
        B1_SERIAL_NET_KEEP_TCP ? shared_names[s_state.phase] : names[s_state.phase],
        (unsigned)ip4_addr1(&address), (unsigned)ip4_addr2(&address),
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

int handoff_mcu_busy(void)
{
    return s_stopping || s_state.phase != HANDOFF_NORMAL || s_reset_stream || slt_reserved();
}

void handoff_mcu_stop(void)
{
    taskENTER_CRITICAL();
    s_stopping = 1U;
    s_active = 0U;
    s_command = HANDOFF_COMMAND_NONE;
    s_slt.waiting_off = 0U;
    taskEXIT_CRITICAL();
}

int handoff_slt_begin(uint32_t cookie)
{
    pwr_ctrl_state_t power = pwr_ctrl_get_state();
    if (!B1_SERIAL_NET_KEEP_TCP || !cookie || !s_netif || handoff_mcu_busy() ||
        s_command != HANDOFF_COMMAND_NONE || pc_power_service_busy() ||
        pc_test_service_busy() || !rk_task_handoff_idle() ||
        (power != PWR_CTRL_STATE_OFF && power != PWR_CTRL_STATE_ON)) return 0;
    taskENTER_CRITICAL();
    s_slt.cookie = cookie;
    s_slt.waiting_off = 1U;
    s_slt.prepared = s_slt.started = s_slt.finished = s_slt.release_queued = 0U;
    if (power == PWR_CTRL_STATE_OFF) slt_arm_off();
    taskEXIT_CRITICAL();
    return 1;
}

int handoff_slt_prepared(uint32_t cookie)
{
    if (!cookie || cookie != s_slt.cookie || !slt_reserved() || s_slt.waiting_off ||
        s_boot_phase == HANDOFF_BOOT_FAILED || s_baud_unknown ||
        pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) return 0;
    if (!s_slt.prepared) s_slt.prepared_ms = HAL_GetTick();
    s_slt.prepared = 1U;
    return 1;
}

handoff_slt_status_t handoff_slt_status(uint32_t cookie)
{
    if (!cookie || cookie != s_slt.cookie) return HANDOFF_SLT_FAILED;
    if (s_slt.finished && !s_reset_stream && s_state.phase == HANDOFF_NORMAL &&
        pwr_ctrl_get_state() == PWR_CTRL_STATE_OFF &&
        s_uart_baud == SN_BOOT_BAUD && !s_baud_unknown) return HANDOFF_SLT_OFF_READY;
    if (s_boot_phase == HANDOFF_BOOT_FAILED || s_baud_unknown || slt_start_expired())
        return HANDOFF_SLT_FAILED;
    if (s_state.phase == HANDOFF_EDK_IDLE && s_slt.release_queued &&
        uart_tx_idle(UART_PORT_RK_PROTOCOL)) return HANDOFF_SLT_RELEASED;
    if (!s_slt.prepared && s_boot_phase == HANDOFF_BOOT_EDK) return HANDOFF_SLT_EARLY_EDK;
    return HANDOFF_SLT_WAITING;
}

int handoff_slt_cancel(uint32_t cookie)
{
    int cancelled = 0;
    taskENTER_CRITICAL();
    if (cookie && cookie == s_slt.cookie && slt_reserved() &&
        !s_slt.started && !s_state.session && !s_network_pending && !s_baud_unknown &&
        s_uart_baud == SN_BOOT_BAUD) {
        memset(&s_state, 0, sizeof(s_state));
        s_slt.cookie = 0U;
        /* 此前未接管UART、未切速；只让RK任务清旁路解析器，不改变真实电源。 */
        s_reset_stream = 1U;
        cancelled = 1;
    }
    taskEXIT_CRITICAL();
    return cancelled;
}
int handoff_mcu_listen_allowed(void)
{
    return B1_SERIAL_NET_KEEP_TCP || handoff_state_listen(&s_state);
}
int handoff_mcu_power_allowed(int off_command)
{
    return off_command || (!s_reset_stream && !s_baud_unknown &&
        (s_state.phase == HANDOFF_NORMAL ||
         (s_state.phase == HANDOFF_ARMED && s_boot_phase == HANDOFF_BOOT_PROBE)));
}

/* tcpip上下文固定本轮地址；共存模式不改netif、ARP缓存或已有TCP连接。 */
static void network_callback(void *unused)
{
    int ok = 0;
    (void)unused;
    if (s_netif && s_network_operation == NETWORK_WITHDRAW &&
        s_state.phase == HANDOFF_WITHDRAWING && (B1_SERIAL_NET_KEEP_TCP || handoff_eth_tx_idle()) &&
        !ip4_addr_isany_val(*netif_ip4_addr(s_netif))) {
        s_ip = *netif_ip4_addr(s_netif);
        s_mask = *netif_ip4_netmask(s_netif);
        s_gateway = *netif_ip4_gw(s_netif);
        s_saved = 1U;
        if (!B1_SERIAL_NET_KEEP_TCP) {
            ip4_addr_t zero = {0};
            etharp_cleanup_netif(s_netif);
            netif_set_addr(s_netif, &zero, &zero, &zero);
        } else if (netif_is_link_up(s_netif)) {
            /* 将上一版独占模式留下的06地址缓存更新为MCU物理MAC。 */
            (void)etharp_gratuitous(s_netif);
        }
        ok = 1;
    } else if (s_network_operation == NETWORK_DRAIN && !s_tx_pending &&
               (B1_SERIAL_NET_KEEP_TCP || handoff_eth_tx_idle())) {
        ok = 1;
    } else if (s_netif && s_network_operation == NETWORK_RESTORE &&
               !s_tx_pending && (B1_SERIAL_NET_KEEP_TCP || handoff_eth_tx_idle())) {
        /* 共存只等RK的tcpip回调结束，已入ETH的pbuf由驱动持有引用；
         * 地址不变，无须等PC连续流量排空。独占模式仍须证明DMA全部发完。 */
        if (s_saved) {
            if (!B1_SERIAL_NET_KEEP_TCP) {
                etharp_cleanup_netif(s_netif);
                netif_set_addr(s_netif, &s_ip, &s_mask, &s_gateway);
                if (netif_is_link_up(s_netif)) (void)etharp_gratuitous(s_netif);
            }
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
    int automatic = slt_reserved();
    int idle = (automatic ? s_slt.prepared : !pc_test_service_busy()) &&
        !pc_power_service_busy() && rk_task_handoff_idle();
    handoff_command_t command;
    taskENTER_CRITICAL();
    command = s_command;
    s_command = HANDOFF_COMMAND_NONE;
    taskEXIT_CRITICAL();
    if (command != HANDOFF_COMMAND_NONE) {
        int accepted;
        taskENTER_CRITICAL();
        if (automatic || s_stopping) accepted = 0; /* 正式SLT/0104不受调试命令干扰。 */
        else if (command == HANDOFF_COMMAND_RK) {
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
    if (automatic && !s_stopping && s_slt.waiting_off && off) slt_arm_off();
    if (s_stopping && off) {
        /* 尚未上电的预约也必须撤销，不能依赖seen_on或EDK归还。 */
        if (s_state.phase == HANDOFF_NORMAL) s_reset_stream = 1U;
        else if (s_state.phase == HANDOFF_ARMED || s_state.phase == HANDOFF_WITHDRAWING ||
                 s_state.phase == HANDOFF_OWNED) (void)handoff_state_cancel(&s_state, 1);
    }
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
    if (!s_stopping && handoff_state_can_withdraw(&s_state, connected && !B1_SERIAL_NET_KEEP_TCP, on, idle,
                                   uart_tx_idle(UART_PORT_RK_PROTOCOL)))
        s_state.phase = HANDOFF_WITHDRAWING;
    taskEXIT_CRITICAL();
    if (s_state.phase == HANDOFF_EDK_IDLE && off && !s_network_pending)
        request_network(NETWORK_DRAIN);
    /* 独占模式先关监听；共存模式只准备/排空RK网卡，当前PC连接可持续服务。 */
    if ((B1_SERIAL_NET_KEEP_TCP || (!connected && !listener_open)) && !s_network_pending) {
        if (s_state.phase == HANDOFF_WITHDRAWING) request_network(NETWORK_WITHDRAW);
        if (s_state.phase == HANDOFF_RELEASING) request_network(NETWORK_RESTORE);
    }
    /* 共存模式已有PC连接也代表网络可用，不等待单客户端策略关闭的监听口。 */
    if ((listener_open || (B1_SERIAL_NET_KEEP_TCP && connected)) && s_state.phase == HANDOFF_RESTORED) {
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
    if (B1_SERIAL_NET_KEEP_TCP) {
        if (!s_active || pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) {
            /* 旧ARP缓存仍可能指向06；关闭RK分流后PC也能直接使用现有连接。 */
            return ours && pbuf_take(p, s_netif->hwaddr, sizeof(s_mac)) != ERR_OK;
        }
        if (p->tot_len > SN_ETH_MAX) return 1;
        s_rx.length = p->tot_len;
        if (pbuf_copy_partial(p, s_rx.data, p->tot_len, 0) != p->tot_len) return 1;
        net_mux_rx_t route = net_mux_rx(s_rx.data, s_rx.length, s_netif->hwaddr, s_mac,
                                        (const uint8_t *)&s_ip.addr, B1_PC_TCP_PORT, B1_DEBUG_TCP_PORT);
        if (route == NET_MUX_RK || route == NET_MUX_BOTH) {
            taskENTER_CRITICAL();
            if (s_active) (void)xQueueSend(s_rx_queue, &s_rx, 0U);
            taskEXIT_CRITICAL();
        }
        if (route == NET_MUX_DROP || route == NET_MUX_RK) return 1;
        /* 仅改以太网头，不改变TCP字节、序号或校验和；原pbuf仍交给lwIP。 */
        return ours && pbuf_take(p, s_netif->hwaddr, sizeof(s_mac)) != ERR_OK;
    }
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
    if (B1_SERIAL_NET_KEEP_TCP) {
        net_mux_tx_t route = net_mux_tx(s_tx.data, s_tx.length, s_netif->hwaddr, s_mac,
                                        (const uint8_t *)&s_ip.addr, B1_PC_TCP_PORT, B1_DEBUG_TCP_PORT);
        if (route == NET_MUX_SUPPRESS) return SN_OK;
        if (route != NET_MUX_SEND) return SN_INVALID;
    }
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
        if ((baud != SN_DATA_BAUD && baud != SN_LEGACY_DATA_BAUD) || s_uart_baud != SN_BOOT_BAUD ||
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

/* 唯一RK任务已解出完整SerialNet请求；旁路HELLO与接管后的请求共用同序号缓存。 */
static void respond_serial(void)
{
    if (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) return;
    if (s_boot_phase == HANDOFF_BOOT_PROBE) {
        taskENTER_CRITICAL();
        if (s_state.phase != HANDOFF_ARMED || s_request.op != SN_HELLO ||
            s_request.status != 0U || s_request.length != 0U || !s_request.session) {
            taskEXIT_CRITICAL();
            return;
        }
        if (slt_reserved()) s_slt.started = 1U;
        s_boot_phase = HANDOFF_BOOT_EDK;
        taskEXIT_CRITICAL();
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
    if (queued && slt_reserved() && handled >= 0 &&
        s_request.op == SN_RELEASE && s_response.status == SN_OK)
        s_slt.release_queued = 1U;
    if (queued && s_uart_baud == SN_BOOT_BAUD && handled >= 0 &&
        s_request.op == SN_HELLO && s_request.length == SN_RATE_REQUEST_SIZE &&
        s_response.status == SN_OK && s_response.length == SN_RATE_REQUEST_SIZE) {
        s_requested_baud = (uint32_t)s_response.data[0] | (uint32_t)s_response.data[1] << 8 |
                           (uint32_t)s_response.data[2] << 16 | (uint32_t)s_response.data[3] << 24;
        s_rate_pending = 1U;
    }
}

int handoff_mcu_rk_observe(uint8_t byte)
{
    if (!slt_reserved() || s_slt.waiting_off || s_reset_stream ||
        pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) return 0;
    uint32_t now = HAL_GetTick();
    if ((uint32_t)(now - s_last_byte) > HANDOFF_PARSER_IDLE_MS) sn_parser_reset(&s_parser);
    s_last_byte = now;
    if (sn_feed(&s_parser, byte, &s_request) == 1) respond_serial();
    return s_boot_phase == HANDOFF_BOOT_EDK;
}

void handoff_mcu_rk_reset_probe(void)
{
    /* 正常TLV尾部不属于COBS请求；接收断流也不能留下半个HELLO。 */
    sn_parser_reset(&s_parser);
}

/* 仅在本轮SPL心跳确认、业务与UART空闲后发一次；入队失败也不自动重发。 */
static void continue_spl_boot(void)
{
    int automatic = slt_reserved();
    if (s_state.phase != HANDOFF_ARMED || s_boot_phase != HANDOFF_BOOT_PROBE ||
        pwr_ctrl_get_state() != PWR_CTRL_STATE_ON || pc_power_service_busy() ||
        (automatic ? !s_slt.prepared : pc_test_service_busy()) || !rk_task_handoff_idle() ||
        rk_link_ready_poll(automatic ? s_slt.cookie : HANDOFF_SPL_READY_TOKEN) != 1 ||
        !uart_tx_idle(UART_PORT_RK_PROTOCOL)) return;
    uint8_t wire[TLV_CMD_FRAME_SIZE];
    taskENTER_CRITICAL();
    /* TCP可能刚撤销尚未执行的预约；发送前再次确认，并先标记可能进入EDK。 */
    if (s_state.phase != HANDOFF_ARMED || s_boot_phase != HANDOFF_BOOT_PROBE ||
        (automatic && (!slt_reserved() || !s_slt.prepared))) {
        taskEXIT_CRITICAL();
        return;
    }
    if (automatic) s_slt.started = 1U;
    s_boot_phase = HANDOFF_BOOT_WAIT_TX;
    taskEXIT_CRITICAL();
    int length = tlv_pack_cmd(HANDOFF_RK_CMD_CONTINUE_BOOT, NULL, 0U,
                             rk_link_next_sequence(), wire, sizeof(wire));
    int queued = length == TLV_CMD_FRAME_SIZE &&
        uart_send(UART_PORT_RK_PROTOCOL, wire, (uint16_t)length) == 0;
    rk_trace_sent(HANDOFF_RK_CMD_CONTINUE_BOOT, wire, length, queued);
    if (!queued) {
        s_boot_phase = HANDOFF_BOOT_FAILED;
        if (automatic) s_slt.started = 0U; /* 整帧未入队，仍可撤销未执行的预约。 */
    }
}

void handoff_mcu_rk_wait(void)
{
    /* 数据期由RX队列唤醒；控制/故障期可能不消费RX，仍须让出CPU，不能被旧块忙等。 */
    if (s_boot_phase == HANDOFF_BOOT_EDK && !s_rate_pending && !s_reset_stream)
        uart_wait_rx(UART_PORT_RK_PROTOCOL, HANDOFF_IDLE_WAIT_MS);
    else
        vTaskDelay(pdMS_TO_TICKS(HANDOFF_IDLE_WAIT_MS));
}

int handoff_mcu_rk_poll(void)
{
    if (s_reset_stream) {
        /* 确定OFF后的新一轮必须先恢复115200，再允许读取SPL协议。 */
        if (s_uart_baud != SN_BOOT_BAUD || s_baud_unknown || s_stopping) {
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
            (void)rk_link_ready_begin(slt_reserved() ? s_slt.cookie : HANDOFF_SPL_READY_TOKEN);
        xQueueReset(s_rx_queue);
        s_reset_stream = 0U;
        if (slt_reserved() && s_state.phase == HANDOFF_NORMAL &&
            pwr_ctrl_get_state() == PWR_CTRL_STATE_OFF) s_slt.finished = 1U;
        if (s_state.phase == HANDOFF_NORMAL && pwr_ctrl_get_state() == PWR_CTRL_STATE_OFF)
            s_stopping = 0U;
    }
    if (s_stopping) return 1;
    if (!handoff_mcu_busy()) return 0;
    if (slt_start_expired()) s_boot_phase = HANDOFF_BOOT_FAILED;
    /* 预留不抢走原RK消费路径；实际识别EDK才停止向后级发送SPL时序/心跳。 */
    if (slt_reserved() && (s_slt.waiting_off ||
        (!s_slt.prepared && s_boot_phase == HANDOFF_BOOT_PROBE))) return 0;
    if (s_rate_pending) {
        if (!uart_tx_idle(UART_PORT_RK_PROTOCOL)) return 1;
        s_rate_pending = 0U;
        if (uart_set_baud(UART_PORT_RK_PROTOCOL, s_requested_baud) != IF_OK) {
            s_baud_unknown = 1U; /* HAL或RX重启失败时不能假设硬件还在旧速率。 */
            s_boot_phase = HANDOFF_BOOT_FAILED;
            report_baud("switch failed; power cycle RK to retry");
            return 1;
        }
        s_uart_baud = s_requested_baud;
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
            respond_serial();
            if (s_rate_pending) return 1;
        }
    }
    (void)tlv_sm_check_timeout(&s_spl_parser, RK_BYTE_TIMEOUT_MS);
    continue_spl_boot();
    if (s_state.phase == HANDOFF_ARMED && s_boot_phase == HANDOFF_BOOT_PROBE)
        rk_task_handoff_heartbeat();
    return 1;
}
