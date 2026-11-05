/* 状态读取和串口是桩；不连接板卡、不把报告输出当成器件验收。 */
#include "b1_diagnostics_port.h"
#include "board_diagnostics.h"
#include "log_console.h"
#include "result_led.h"
#include <string.h>

#define STR1(x) #x
#define STR(x) STR1(x)
#define CHECK(x) do { if (!(x)) fail("FAIL " STR(__LINE__) ": " #x "\n"); } while (0)
static void message(const char *text)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = text;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}
static _Noreturn void finish(uint32_t code)
{
    const uint32_t block[] = {0x20026U, code};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register const uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) {}
}
static _Noreturn void fail(const char *text) { message(text); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }
GPIO_TypeDef test_gpio;
GPIO_TypeDef test_gpio_g;
static result_led_state_t led_states[RESULT_LED_TARGET_COUNT];
UART_HandleTypeDef huart1;
struct netif gnetif = {{192,168,1,201}, {2,3,4,5,6,7}, 1, 0};
static uint32_t tick, isr, primask, basepri;
static int critical, requested, blocked, selected = 1, has_sample = 1;
static unsigned calls, boot_calls;
static char output[4096];
static size_t used;
static monitor_snapshot_t snapshot;
static void clear_output(void) { used = 0; output[0] = 0; }
static void append(const char *data, size_t length)
{
    CHECK(used + length < sizeof(output));
    memcpy(output + used, data, length);
    used += length; output[used] = 0;
}
uint32_t __get_IPSR(void) { return isr; }
uint32_t __get_PRIMASK(void) { return primask; }
uint32_t __get_BASEPRI(void) { return basepri; }
uint32_t HAL_GetTick(void) { return tick; }
int HAL_UART_Transmit(UART_HandleTypeDef *uart, const uint8_t *data, uint16_t length, uint32_t timeout)
{
    CHECK(uart == &huart1 && timeout == 20U && !isr && !primask && !basepri);
    ++boot_calls; append((const char *)data, length); return 0;
}
void test_enter(void) { ++critical; }
void test_exit(void) { CHECK(critical == 1); --critical; }
size_t xPortGetFreeHeapSize(void) { return 65536; }
uint8_t board_slot_code(void) { return 7; }
uint8_t b2_service_site_id(void) { return 0; }
HAL_StatusTypeDef result_led_get_all(result_led_state_t states[RESULT_LED_TARGET_COUNT])
{
    CHECK(critical == 1);
    memcpy(states, led_states, sizeof(led_states));
    return HAL_OK;
}
pwr_ctrl_state_t pwr_ctrl_get_state(void) { return snapshot.power_state; }
int monitor_get_latest(monitor_snapshot_t *out) { *out = snapshot; return has_sample; }
int monitor_get_diagnostics(monitor_diagnostics_t *out)
{
    memset(out, 0, sizeof(*out));
    out->adc_id = 7;
    out->adc_init_stage = 6U; out->adc_init_channel = 1U;
    out->adc_init_result = -1; out->adc_error_read = 0x5000U;
    out->adc_cal_snapshot.valid = 1U;
    out->adc_cal_snapshot.control = 0x0790U;
    out->adc_cal_snapshot.channel = 0x8001U;
    out->adc_cal_snapshot.config = 0x0010U;
    out->adc_cal_snapshot.filter = 0x000180U;
    out->adc_cal_snapshot.error_enable = 0x07F2F8U;
    out->adc_cal_snapshot.error = 0x1000U;
    return 1;
}
int uart_get_rx_diagnostics(int port, uart_rx_diagnostics_t *out)
{ memset(out, 0, sizeof(*out)); out->errors = (unsigned)port; return 1; }
void b2_spi_diagnostics(b2_spi_diagnostics_t *out) { memset(out, 0, sizeof(*out)); }
void b2_link_diagnostics(b2_link_diagnostics_t *out) { memset(out, 0, sizeof(*out)); }
int rk_link_get_rx_diagnostics(rk_rx_diagnostics_t *out)
{ memset(out, 0, sizeof(*out)); return 0; }
int log_console_take_status_request(void)
{ int value = requested; requested = 0; return value; }
int log_console_write_status(const char *text, size_t length)
{
    CHECK(!critical && length > 0 && length <= 96);
    ++calls;
    if (!selected) return -1;
    if (blocked) return 0;
    append(text, length); return 1;
}
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; ++i) { ++tick; board_diagnostics_process(); }
}
void test_main(void)
{
    board_diagnostics_boot_stage("before UART");
    board_diagnostics_boot_error(); CHECK(boot_calls == 0);
    board_diagnostics_boot_console_ready();
    CHECK((boot_calls != 0) == (B1_LOG_DEFAULT_SOURCE == 0));
    clear_output();
    board_diagnostics_boot_stage("W25 device init");
    board_diagnostics_boot_error();
    CHECK(B1_LOG_DEFAULT_SOURCE != 0 || strstr(output, "FAILED at W25 device init") != NULL);
    unsigned saved = boot_calls;
    isr = 1; board_diagnostics_boot_error(); isr = 0;
    primask = 1; board_diagnostics_boot_error(); primask = 0;
    basepri = 1; board_diagnostics_boot_error(); basepri = 0;
    CHECK(boot_calls == saved);
    board_diagnostics_boot_finish(); saved = boot_calls;
    board_diagnostics_boot_console_ready(); board_diagnostics_boot_error();
    CHECK(boot_calls == saved); /* 运行期绝不能再次直接写UART。 */

    clear_output();
    tick = 1499; board_diagnostics_process(); CHECK(used == 0);
    advance(301);
    CHECK(strstr(output, "code=7 candidateSite=8 assignedSite=0") != NULL);
    CHECK(strstr(output, "IP=192.168.1.201 up=1 link=0") != NULL);
    CHECK(strstr(output, "[VDD1/2/Q mV] unavailable") != NULL);
    CHECK(strstr(output, "[AD reference] configured=AVDD assumedMv=3333") != NULL);
    CHECK(strstr(output, "[IDD1/2/Q uA] unavailable") != NULL);
    CHECK(strstr(output, "[TPS status 1/2/Q] unavailable") != NULL);
    CHECK(strstr(output, "[LED] states=0,0,0 PG0..6 ODR=0x00 IDR=0x00") != NULL);
    CHECK(strstr(output, "[status] end") != NULL);
    saved = calls; advance(100); CHECK(calls == saved); /* 不周期刷屏。 */

    snapshot.memory.valid_mask = 7; snapshot.power_state = PWR_CTRL_STATE_ON;
    snapshot.memory.voltage_mv[0] = 1800; snapshot.memory.voltage_mv[1] = 1100;
    snapshot.memory.voltage_mv[2] = 600; snapshot.memory.idd[0].current_ua = 123456;
    snapshot.memory.idd[1].current_ua = 234567; snapshot.memory.idd[2].current_ua = 345678;
    snapshot.timestamp_ms = tick; test_gpio.ODR = 192;
    led_states[0] = RESULT_LED_TEST_NORMAL;
    led_states[1] = RESULT_LED_OFF;
    led_states[2] = RESULT_LED_PASS;
    test_gpio_g.ODR = 0x63U; test_gpio_g.IDR = 0x43U; test_gpio_g.MODER = 0x1555U;
    clear_output(); requested = 1; advance(300);
    CHECK(strstr(output, "state=ON PC6=1 PC7=1") != NULL);
    CHECK(strstr(output, "[AD init] stage=calibration channel=1 result=-1") != NULL);
    CHECK(strstr(output, "errorRead=0x005000") != NULL);
    CHECK(strstr(output, "[AD preCal] valid=1 ctrl=0x0790 ch=0x8001 cfg=0x0010") != NULL);
    CHECK(strstr(output, "filter=0x000180 errorEn=0x07F2F8") != NULL);
    CHECK(strstr(output, "error=0x001000") != NULL);
    CHECK(strstr(output, "[LED] states=3,0,1 PG0..6 ODR=0x63 IDR=0x43 MODER=0x1555") != NULL);
    CHECK(strstr(output, "[VDD1/2/Q mV] 1800 1100 600") != NULL);
    CHECK(strstr(output, "[IDD1/2/Q uA] 123456 234567 345678") != NULL);
    CHECK(strstr(output, "[status] end") != NULL);

    clear_output(); requested = 1; blocked = 1; saved = calls; advance(55);
    CHECK(used == 0 && calls - saved <= 6); /* 拥塞时有间隔重试。 */
    blocked = 0; advance(250); CHECK(strstr(output, "[status] end") != NULL);
    clear_output(); requested = 1; blocked = 1; advance(2100);
    blocked = 0; advance(200); CHECK(used == 0); /* 超过2秒丢弃，不在后来补发旧报告。 */

    clear_output(); requested = 1; advance(21); CHECK(used > 0);
    selected = 0; advance(30); clear_output();
    selected = 1; advance(200); CHECK(used == 0); /* 切换来源后没有报告残尾。 */
    has_sample = 0; requested = 1; advance(300);
    CHECK(strstr(output, "not published yet") != NULL);
    CHECK(strstr(output, "[status] end") != NULL && !critical);
    message("PASS: B1 boot diagnostics and bounded status report\n");
    finish(0);
}
