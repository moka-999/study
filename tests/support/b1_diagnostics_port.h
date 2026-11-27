/* 仅替换诊断模块的硬件/任务读取接口；实际格式化、限速和启动门禁在正式C中执行。 */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t ODR, IDR, MODER; } GPIO_TypeDef;
extern GPIO_TypeDef test_gpio;
extern GPIO_TypeDef test_gpio_g;
#define GPIOC (&test_gpio)
#define GPIOG (&test_gpio_g)
typedef enum { HAL_OK, HAL_ERROR } HAL_StatusTypeDef;
#define B1_PWR_4V0_EN_PIN 64U
#define B1_PWR_3V3_EN_PIN 128U
typedef struct { uint32_t unused; } UART_HandleTypeDef;
extern UART_HandleTypeDef huart1;
uint32_t __get_IPSR(void);
uint32_t __get_PRIMASK(void);
uint32_t __get_BASEPRI(void);
uint32_t HAL_GetTick(void);
int HAL_UART_Transmit(UART_HandleTypeDef *, const uint8_t *, uint16_t, uint32_t);
void test_enter(void);
void test_exit(void);
#define taskENTER_CRITICAL() test_enter()
#define taskEXIT_CRITICAL() test_exit()
typedef struct {
    size_t xAvailableHeapSpaceInBytes, xMinimumEverFreeBytesRemaining, xSizeOfLargestFreeBlockInBytes;
} HeapStats_t;
void vPortGetHeapStats(HeapStats_t *stats);
typedef const char *TaskHandle_t;
typedef uint32_t StackType_t;
#define TCPIP_THREAD_NAME "tcpip_thread"
TaskHandle_t xTaskGetHandle(const char *name);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);
uint8_t board_slot_code(void);
uint8_t b2_service_site_id(void);
typedef enum {
    PWR_CTRL_STATE_OFF, PWR_CTRL_STATE_WAIT_FIRST_RAIL, PWR_CTRL_STATE_WAIT_SECOND_RAIL,
    PWR_CTRL_STATE_ON, PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF, PWR_CTRL_STATE_FAULT
} pwr_ctrl_state_t;
pwr_ctrl_state_t pwr_ctrl_get_state(void);
#define MEMORY_MONITOR_VALID_VOLTAGE 1U
#define MEMORY_MONITOR_VALID_IDD 2U
#define MEMORY_MONITOR_VALID_TPS_STATUS 4U
typedef struct {
    uint8_t valid_mask;
    int32_t voltage_mv[3];
    struct { uint32_t vbus_mv; uint64_t current_ua; } idd[3];
    uint8_t tps_status[3];
} memory_monitor_sample_t;
typedef struct {
    uint32_t timestamp_ms;
    pwr_ctrl_state_t power_state;
    memory_monitor_sample_t memory;
} monitor_snapshot_t;
typedef struct {
    uint16_t control, channel, config;
    uint32_t filter, error_enable, error;
    uint8_t status, valid;
} ad7124_cal_snapshot_t;
typedef struct {
    uint8_t adc_ready, pac_ready, adc_id, pac_last_error;
    uint32_t adc_last_error;
    uint32_t adc_error_read;
    unsigned adc_init_stage;
    int adc_init_result;
    uint8_t adc_init_channel, adc_status;
    ad7124_cal_snapshot_t adc_cal_snapshot;
} monitor_diagnostics_t;
int monitor_get_latest(monitor_snapshot_t *);
int monitor_get_diagnostics(monitor_diagnostics_t *);
#define UART_PORT_COUNT 3
#define UART_PORT_RK_PROTOCOL 0
typedef struct {
    uint32_t errors, last_hal_error, dropped_blocks, restart_failures, queue_overflows;
    uint32_t last_drop_ms, session_end_bytes, invalid_events, recoveries;
    uint8_t recovery_pending;
} uart_rx_diagnostics_t;
int uart_get_rx_diagnostics(int, uart_rx_diagnostics_t *);
typedef struct {
    uint32_t transactions, received_requests, hardware_errors, queue_drops;
} b2_spi_diagnostics_t;
typedef struct { uint32_t valid_requests, invalid_frames; uint8_t online; } b2_link_diagnostics_t;
void b2_spi_diagnostics(b2_spi_diagnostics_t *);
void b2_link_diagnostics(b2_link_diagnostics_t *);
typedef struct { uint32_t rejected_total, last_reason, last_tick; } rk_rx_diagnostics_t;
int rk_link_get_rx_diagnostics(rk_rx_diagnostics_t *);
struct netif { uint8_t ip[4], hwaddr[6], up, link; };
#define netif_ip4_addr(n) ((n)->ip)
#define ip4_addr1(p) ((p)[0])
#define ip4_addr2(p) ((p)[1])
#define ip4_addr3(p) ((p)[2])
#define ip4_addr4(p) ((p)[3])
#define netif_is_up(n) ((n)->up)
#define netif_is_link_up(n) ((n)->link)
