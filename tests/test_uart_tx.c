/* 实际发送环测试：TXE只搬入硬件，TC才完成；后续载荷不能越过跟踪帧边界。 */
#include "port.h"
#include "uart_under_test.h"
#define S1(x) #x
#define S(x) S1(x)
#define CHECK(x) do { if (!(x)) fail("FAIL " S(__LINE__) ": " #x "\n"); } while (0)
static USART_TypeDef registers[3];
UART_HandleTypeDef huart1 = {.Instance = &registers[0]};
UART_HandleTypeDef huart2 = {.Instance = &registers[1]};
UART_HandleTypeDef huart4 = {.Instance = &registers[2]};
static uint32_t tick, primask;
static int locks[3], lock_count, lock_failure;
static int init_calls, init_failures, abort_failure;
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
    for (;;) { }
}
static _Noreturn void fail(const char *text) { message(text); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }
uint32_t HAL_GetTick(void) { return tick; }
uint32_t __get_PRIMASK(void) { return primask; }
void __disable_irq(void) { primask = 1; }
void __set_PRIMASK(uint32_t saved) { primask = saved; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { CHECK(lock_count < 3); return &locks[lock_count++]; }
int xSemaphoreTake(SemaphoreHandle_t lock, TickType_t wait)
{ (void)wait; CHECK(!*lock); if (lock_failure) return 0; *lock = 1; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t lock) { CHECK(*lock); *lock = 0; return pdTRUE; }
int HAL_UART_AbortReceive(UART_HandleTypeDef *uart)
{ (void)uart; CHECK(!primask); return abort_failure ? HAL_ERROR : HAL_OK; }
int HAL_UART_Init(UART_HandleTypeDef *uart)
{
    CHECK(!primask);
    ++init_calls; uart->TxISR = NULL;
    if (init_failures) { --init_failures; return HAL_ERROR; }
    return HAL_OK;
}
int HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef *uart, uint8_t *data, uint16_t size)
{ (void)uart; (void)data; (void)size; return HAL_OK; }
static void txe(UART_HandleTypeDef *uart)
{
    CHECK(uart->TxISR != NULL && (uart->Instance->CR1 & UART_IT_TXE));
    uart->Instance->ISR = USART_ISR_TXE_TXFNF;
    uart->TxISR(uart);
}
static void tc(UART_HandleTypeDef *uart)
{
    CHECK(uart->Instance->CR1 & UART_IT_TC);
    /* 对应正式HAL的UART_EndTransmit_IT：先清TCIE/TxISR，再调用完成钩子。 */
    uart->Instance->CR1 &= ~UART_IT_TC;
    uart->TxISR = NULL;
    HAL_UART_TxCpltCallback(uart);
}
void test_main(void)
{
    uint8_t frame[] = {1, 2, 3}, later[] = {4, 5};
    uint32_t ticket = 77U, other = 88U, completed = 99U;
    CHECK(uart_hw_send_tracked(&uart1_if, frame, sizeof(frame), &ticket) == IF_ERR);
    CHECK(ticket == 77U);
    uart_tx_mutex_init(); uart_tx_isr_attach();
    CHECK(uart_hw_send_tracked(&uart1_if, frame, sizeof(frame), &ticket) == IF_OK);
    CHECK(uart_hw_tx_status(&uart1_if, ticket, &completed) == 0 && completed == 99U);
    CHECK(uart_hw_send_tracked(&uart1_if, frame, sizeof(frame), &other) == IF_ERR && other == 88U);
    CHECK(uart1_if.send(&uart1_if, later, sizeof(later)) == IF_OK);
    txe(&huart1);
    CHECK(registers[0].TDR == 3U && !(registers[0].CR1 & UART_IT_TXE));
    CHECK(uart_hw_tx_status(&uart1_if, ticket, &completed) == 0);
    CHECK(uart1_if.send(&uart1_if, later, sizeof(later)) == IF_OK);
    CHECK(!(registers[0].CR1 & UART_IT_TXE));
    tick = UINT32_MAX; tc(&huart1);
    CHECK(uart_hw_tx_status(&uart1_if, ticket, &completed) == 1 && completed == UINT32_MAX);
    CHECK(huart1.TxISR != NULL && (registers[0].CR1 & UART_IT_TXE));
    txe(&huart1);
    CHECK(registers[0].TDR == 5U && !(registers[0].CR1 & UART_IT_TXE));
    CHECK(uart_hw_tx_status(&uart1_if, ticket, &completed) == 1);
    /* 绕回环尾；普通数据已入队但尚未发完时，跟踪边界仍准确。 */
    static uint8_t bulk[2040];
    CHECK(uart1_if.send(&uart1_if, bulk, sizeof(bulk)) == IF_OK);
    primask = 1U;
    CHECK(uart_hw_send_tracked(&uart1_if, frame, sizeof(frame), &other) == IF_OK && primask == 1U);
    primask = 0U;
    CHECK(uart_hw_tx_status(&uart1_if, ticket, &completed) == IF_ERR);
    txe(&huart1);
    CHECK(registers[0].TDR == 3U && uart_hw_tx_status(&uart1_if, other, &completed) == 0);
    tick = 3U; tc(&huart1);
    CHECK(uart_hw_tx_status(&uart1_if, other, &completed) == 1 && completed == 3U);
    lock_failure = 1;
    CHECK(uart_hw_send_tracked(&uart1_if, frame, sizeof(frame), &ticket) == IF_ERR);
    lock_failure = 0;
    static uint8_t full[2047];
    CHECK(uart1_if.send(&uart1_if, full, sizeof(full)) == IF_OK);
    CHECK(uart_hw_send_tracked(&uart1_if, frame, sizeof(frame), &ticket) == IF_ERR);
    CHECK(uart_hw_tx_status(&uart1_if, other, &completed) == 1);
    txe(&huart1);
    CHECK(uart_hw_send_tracked(NULL, frame, sizeof(frame), &ticket) == IF_ERR);
    CHECK(uart_hw_send_tracked(&uart1_if, frame, sizeof(frame), NULL) == IF_ERR);
    CHECK(uart_hw_tx_status(&uart1_if, 0U, &completed) == IF_ERR);
#if defined(TEST_B1)
    /* B1协议实际绑定UART4，与USART1控制台的完成票据互不干扰。 */
    CHECK(uart_hw_send_tracked(&uart4_if, frame, sizeof(frame), &ticket) == IF_OK);
    txe(&huart4);
    CHECK(uart_hw_tx_status(&uart4_if, ticket, &completed) == 0);
    tick = 12U; tc(&huart4);
    CHECK(uart_hw_tx_status(&uart4_if, ticket, &completed) == 1 && completed == 12U);
    CHECK(uart_hw_tx_status(&uart1_if, other, &completed) == 1 && completed == 3U);
#endif
#if defined(TEST_B1_HANDOFF)
    /* 改速只能在TX环和TC同时空闲时执行；HAL失败恢复原值，重挂本端TX钩子。 */
    huart4.Init.BaudRate = 115200U;
    registers[2].ISR = 0U;
    CHECK(uart_hw_set_baud(&uart4_if, 2000000U) == IF_ERR && init_calls == 0);
    registers[2].ISR = UART_FLAG_TC;
    CHECK(uart_hw_set_baud(&uart4_if, 2000000U) == IF_OK && huart4.Init.BaudRate == 2000000U);
    CHECK(huart4.TxISR != NULL && init_calls == 1);
    init_failures = 1;
    CHECK(uart_hw_set_baud(&uart4_if, 115200U) == IF_ERR && huart4.Init.BaudRate == 2000000U);
    CHECK(huart4.TxISR != NULL && init_calls == 3);
    abort_failure = 1;
    CHECK(uart_hw_set_baud(&uart4_if, 115200U) == IF_ERR && init_calls == 3);
    abort_failure = 0;
    CHECK(uart_hw_set_baud(&uart4_if, 115200U) == IF_OK && huart4.Init.BaudRate == 115200U);
    message("PASS: UART actual baud switch, TX drain, RX abort failure and HAL rollback\n");
#endif
    message("PASS: UART tracked TC, queued boundary, ring wrap, failures and IRQ mask preservation\n");
    finish(0U);
}
