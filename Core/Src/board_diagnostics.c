#include "board_diagnostics.h"
#include "board_adc_config.h"
#include "log_console.h"
#include "protocol_trace.h"
#include "main.h"
#include "board_identity.h"
#include "monitor_task.h"
#include "uart_router.h"
#include "b2_spi.h"
#include "b2_task.h"
#include "b2_service.h"
#include "rk_link.h"
#include "result_led.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/netif.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern struct netif gnetif;

/* 字符串来自静态字面量；即使UART未就绪，SWD仍可查看最后到达的阶段。 */
static const char * volatile s_boot_stage = "reset";
static uint8_t s_boot_console_ready;
static uint8_t s_boot_finished;

/* 只允许调度前、SysTick可运行时使用HAL的20ms超时。
 * 退出早期阶段后永不重启此出口，避免与USART1的中断发送环争用。 */
static void boot_write(const char *text)
{
    if (!s_boot_console_ready || s_boot_finished || B1_LOG_DEFAULT_SOURCE != 0 ||
        __get_IPSR() != 0U || __get_PRIMASK() != 0U || __get_BASEPRI() != 0U) return;
    (void)HAL_UART_Transmit(&huart1, (const uint8_t *)text, (uint16_t)strlen(text), 20U);
}

void board_diagnostics_boot_stage(const char *stage)
{
    if (stage == NULL || s_boot_finished) return;
    s_boot_stage = stage;
    boot_write("[boot] ");
    boot_write(stage);
    boot_write("\r\n");
}

void board_diagnostics_boot_console_ready(void)
{
    if (s_boot_finished) return;
    s_boot_console_ready = 1U;
    board_diagnostics_boot_stage("USART1 ready 115200/8N1; RK/DDR power OFF");
}

void board_diagnostics_boot_finish(void)
{
    board_diagnostics_boot_stage("RTOS create/start");
    s_boot_finished = 1U;
}

void board_diagnostics_boot_error(void)
{
    boot_write("[boot] FAILED at ");
    boot_write(s_boot_stage);
    boot_write("\r\n");
}

/* 报告缓冲为静态内存，不分配堆；96字节/10ms限速低于115200波特率的吞吐。
 * 出口忙最多等待2秒后丢弃报告，不阻塞采样/电源/RK任务，不影响故障处置。 */
/* 为三类高频协议计数和串口丢弃统计保留快照空间。 */
#define STATUS_CAPACITY 2560U
#define STATUS_CHUNK_SIZE 96U
#define STATUS_INTERVAL_MS 10U
#define STATUS_DEADLINE_MS 2000U
static char s_report[STATUS_CAPACITY];
static size_t s_report_length;
static size_t s_report_offset;
static uint32_t s_report_started;
static uint32_t s_last_send;
static uint8_t s_initial_report = 1U;

static void report_append(const char *format, ...)
{
    if (s_report_length >= sizeof(s_report) - 1U) return;
    size_t remaining = sizeof(s_report) - s_report_length;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(s_report + s_report_length, remaining, format, arguments);
    va_end(arguments);
    if (written > 0) {
        s_report_length += (size_t)written < remaining ? (size_t)written : remaining - 1U;
    }
}

/* GPIO锁存是软件输出命令，不是电源轨实测反馈；ON也不代表RK算法已就绪。 */
static const char *power_name(pwr_ctrl_state_t state)
{
    static const char * const names[] = {
        "OFF", "WAIT_FIRST_RAIL", "WAIT_SECOND_RAIL", "ON", "WAIT_OFF", "FAULT"
    };
    return (unsigned int)state < sizeof(names) / sizeof(names[0]) ? names[state] : "UNKNOWN";
}

static void build_report(uint32_t now)
{
    monitor_snapshot_t snapshot = {0};
    monitor_diagnostics_t monitor = {0};
    b2_spi_diagnostics_t spi = {0};
    b2_link_diagnostics_t b2 = {0};
    rk_rx_diagnostics_t rk = {0};
    uint8_t ip[4], mac[6], up, link;
    uint32_t outputs, led_outputs, led_inputs, led_modes;
    result_led_state_t led_states[RESULT_LED_TARGET_COUNT] = {0};
    HAL_StatusTypeDef led_status;
    uint8_t slot = board_slot_code();
    s_report_length = 0U;
    s_report_offset = 0U;
    s_report_started = now;
    s_last_send = now - STATUS_INTERVAL_MS;

    /* netif由网络线程更新；只在短临界区复制，不在其中格式化或发串口。 */
    taskENTER_CRITICAL();
    ip[0] = ip4_addr1(netif_ip4_addr(&gnetif));
    ip[1] = ip4_addr2(netif_ip4_addr(&gnetif));
    ip[2] = ip4_addr3(netif_ip4_addr(&gnetif));
    ip[3] = ip4_addr4(netif_ip4_addr(&gnetif));
    memcpy(mac, gnetif.hwaddr, sizeof(mac));
    up = netif_is_up(&gnetif) ? 1U : 0U;
    link = netif_is_link_up(&gnetif) ? 1U : 0U;
    outputs = GPIOC->ODR;
    /* 同一快照读取业务状态、输出锁存和引脚电平；不改变灯态。 */
    led_status = result_led_get_all(led_states);
    led_outputs = GPIOG->ODR & 0x7FU;
    led_inputs = GPIOG->IDR & 0x7FU;
    led_modes = GPIOG->MODER & 0x3FFFU;
    taskEXIT_CRITICAL();

    report_append("\r\n[status] uptimeMs=%lu freeHeap=%lu\r\n",
                  (unsigned long)now, (unsigned long)xPortGetFreeHeapSize());
    report_append("[slot] code=%u candidateSite=%u assignedSite=%u (0=unassigned)\r\n",
                  (unsigned int)slot, (unsigned int)slot + 1U,
                  (unsigned int)b2_service_site_id());
    report_append("[net] IP=%u.%u.%u.%u up=%u link=%u MAC=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                  ip[0], ip[1], ip[2], ip[3], up, link,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    report_append("[power] state=%s PC6=%u PC7=%u (outputs, not measured rails)\r\n",
                  power_name(pwr_ctrl_get_state()),
                  (outputs & B1_PWR_4V0_EN_PIN) != 0U,
                  (outputs & B1_PWR_3V3_EN_PIN) != 0U);
    if (led_status == HAL_OK) {
        report_append("[LED] states=%u,%u,%u PG0..6 ODR=0x%02lX IDR=0x%02lX MODER=0x%04lX\r\n",
                      (unsigned)led_states[0], (unsigned)led_states[1], (unsigned)led_states[2],
                      (unsigned long)led_outputs, (unsigned long)led_inputs, (unsigned long)led_modes);
    } else report_append("[LED] state unavailable\r\n");
    (void)monitor_get_diagnostics(&monitor);
    report_append("[monitor] ADready=%u ADid=0x%02X ADlastError=0x%06lX PACready=%u PAClastError=%u\r\n",
                  monitor.adc_ready, monitor.adc_id, (unsigned long)monitor.adc_last_error,
                  monitor.pac_ready, monitor.pac_last_error);
    /* 仅显示任务发布的快照；255表示无校准通道，不在日志路径访问SPI。 */
    static const char *const ad_stages[] = {
        "none", "reset", "id", "control", "config", "diagnostics",
        "calibration", "continuous", "ready"
    };
    unsigned ad_stage = (unsigned)monitor.adc_init_stage;
    report_append("[AD init] stage=%s channel=%u result=%d status=0x%02X errorRead=0x%06lX\r\n",
                  ad_stage < sizeof(ad_stages) / sizeof(ad_stages[0]) ? ad_stages[ad_stage] : "unknown",
                  monitor.adc_init_channel, monitor.adc_init_result, monitor.adc_status,
                  (unsigned long)monitor.adc_error_read);
    report_append("[AD reference] configured=%s assumedMv=%u\r\n",
                  BOARD_AD_USE_AVDD_REFERENCE ? "AVDD" : "internal",
                  (unsigned)BOARD_AD_REFERENCE_MV);
    const ad7124_cal_snapshot_t *cal = &monitor.adc_cal_snapshot;
    report_append("[AD preCal] valid=%u ctrl=0x%04X ch=0x%04X cfg=0x%04X\r\n",
                  cal->valid, cal->control, cal->channel, cal->config);
    report_append("[AD preCal] filter=0x%06lX errorEn=0x%06lX status=0x%02X error=0x%06lX\r\n",
                  (unsigned long)cal->filter, (unsigned long)cal->error_enable,
                  cal->status, (unsigned long)cal->error);
    if (monitor_get_latest(&snapshot)) {
        report_append("[sample] ageMs=%lu valid=0x%02X (V=1 I=2 TPS=4)\r\n",
                      (unsigned long)(HAL_GetTick() - snapshot.timestamp_ms), snapshot.memory.valid_mask);
        if ((snapshot.memory.valid_mask & MEMORY_MONITOR_VALID_VOLTAGE) != 0U) {
            report_append("[VDD1/2/Q mV] %ld %ld %ld\r\n",
                          (long)snapshot.memory.voltage_mv[0], (long)snapshot.memory.voltage_mv[1],
                          (long)snapshot.memory.voltage_mv[2]);
        } else report_append("[VDD1/2/Q mV] unavailable\r\n");
        if ((snapshot.memory.valid_mask & MEMORY_MONITOR_VALID_IDD) != 0U) {
            /* PAC在B1的20mOhm量程内最大电流远小于UINT32_MAX uA。 */
            report_append("[IDD1/2/Q uA] %lu %lu %lu\r\n",
                          (unsigned long)snapshot.memory.idd[0].current_ua,
                          (unsigned long)snapshot.memory.idd[1].current_ua,
                          (unsigned long)snapshot.memory.idd[2].current_ua);
        } else report_append("[IDD1/2/Q uA] unavailable\r\n");
        if ((snapshot.memory.valid_mask & MEMORY_MONITOR_VALID_TPS_STATUS) != 0U) {
            report_append("[TPS status 1/2/Q] 0x%02X 0x%02X 0x%02X\r\n",
                          snapshot.memory.tps_status[0], snapshot.memory.tps_status[1],
                          snapshot.memory.tps_status[2]);
        } else report_append("[TPS status 1/2/Q] unavailable\r\n");
    } else report_append("[sample] not published yet\r\n");

    static const char * const uart_names[] = {"UART4-RK", "USART2-RKlog", "USART1-console"};
    for (int port = 0; port < UART_PORT_COUNT; ++port) {
        uart_rx_diagnostics_t uart = {0};
        if (uart_get_rx_diagnostics(port, &uart)) {
            report_append("[%s] err=%lu HAL=0x%lX drop=%lu restartFail=%lu recoveryPending=%u\r\n",
                          uart_names[port], (unsigned long)uart.errors,
                          (unsigned long)uart.last_hal_error, (unsigned long)uart.dropped_blocks,
                          (unsigned long)uart.restart_failures, uart.recovery_pending);
        }
    }
    b2_spi_diagnostics(&spi);
    b2_link_diagnostics(&b2);
    report_append("[B2 SPI] cs=%lu rx=%lu hwErr=%lu drop=%lu valid=%lu bad=%lu online=%u\r\n",
                  (unsigned long)spi.transactions, (unsigned long)spi.received_requests,
                  (unsigned long)spi.hardware_errors, (unsigned long)spi.queue_drops,
                  (unsigned long)b2.valid_requests, (unsigned long)b2.invalid_frames, b2.online);
    (void)rk_link_get_rx_diagnostics(&rk);
    report_append("[RK history] rejected=%lu lastReason=%u lastTick=%lu (not current busy)\r\n",
                  (unsigned long)rk.rejected_total, (unsigned int)rk.last_reason,
                  (unsigned long)rk.last_tick);
    trace_report(report_append);
    report_append("[status] end; snapshot only, OFF means sensors may be uninitialized\r\n");
}

void board_diagnostics_process(void)
{
    uint32_t now = HAL_GetTick();
    if (s_report_offset == s_report_length) {
        /* 启动后留1.5秒给监测任务发布OFF快照；RK默认模式会静默取消本次输出。 */
        if (log_console_take_status_request() || (s_initial_report && now >= 1500U)) {
            s_initial_report = 0U;
            build_report(now);
        } else return;
    }
    if (now - s_report_started >= STATUS_DEADLINE_MS) {
        s_report_offset = s_report_length;
        return;
    }
    if (now - s_last_send < STATUS_INTERVAL_MS) return;
    size_t length = s_report_length - s_report_offset;
    if (length > STATUS_CHUNK_SIZE) length = STATUS_CHUNK_SIZE;
    int sent = log_console_write_status(s_report + s_report_offset, length);
    if (sent < 0) s_report_offset = s_report_length; /* 切换来源，取消未发报告。 */
    else if (sent > 0) s_report_offset += length;
    s_last_send = now;
}
