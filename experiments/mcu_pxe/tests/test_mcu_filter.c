/* 使用真实CMSIS/HAL、FreeRTOS/lwIP头及MCU源码；只替代寄存器、队列和电源状态。 */
#include "main.h"
static ETH_TypeDef test_eth;
#undef ETH
#define ETH (&test_eth)
#include "../mcu/serial_net_mcu.c"

#define S0(x) #x
#define S(x) S0(x)
#define CHECK(x) do { if (!(x)) fail("FAIL " S(__LINE__) " " #x "\n"); } while (0)
static void message(const char *s)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = s;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}
static _Noreturn void finish(uint32_t code)
{
    uint32_t block[] = {0x20026U, code};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) {}
}
static _Noreturn void fail(const char *s) { message(s); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }

static uint8_t bytes[SN_ETH_MAX + 1U];
static eth_packet_t queued;
static unsigned queue_calls;
static int queue_accept = 1;
static pwr_ctrl_state_t power_state = PWR_CTRL_STATE_ON;
static struct pbuf tail = {.payload = bytes + 3, .len = 57, .tot_len = 57};
static struct pbuf head = {.next = &tail, .payload = bytes, .len = 3, .tot_len = 60};

pwr_ctrl_state_t pwr_ctrl_get_state(void) { return power_state; }
BaseType_t xQueueGenericSend(QueueHandle_t queue, const void * const item,
                             TickType_t wait, const BaseType_t position)
{
    CHECK(queue == s_rx_queue && wait == 0 && position == queueSEND_TO_BACK);
    ++queue_calls;
    if (!queue_accept) return pdFALSE;
    memcpy(&queued, item, sizeof(queued));
    return pdTRUE;
}

/* 以同一个跨pbuf头部检查真实收包函数：owned=1由调用方消费，入队包须逐字节一致。 */
static void receive_case(const uint8_t dst[6], uint16_t type, int owned, int enqueue)
{
    unsigned before = queue_calls;
    uint32_t received = s_rx_count;
    memcpy(bytes, dst, 6);
    bytes[12] = (uint8_t)(type >> 8); bytes[13] = (uint8_t)type;
    CHECK(serial_net_input(&head) == owned);
    CHECK(queue_calls == before + (unsigned)enqueue);
    CHECK(s_rx_count == received + (unsigned)(enqueue && queue_accept));
    if (enqueue && queue_accept) {
        CHECK(queued.length == head.tot_len);
        CHECK(memcmp(queued.data, bytes, queued.length) == 0);
    }
}

void test_main(void)
{
    struct netif netif = {.hwaddr = {2, 0x2A, 0xDF, 0x95, 0x2D, 4}};
    const uint8_t ours[6] = {6, 0x2A, 0xDF, 0x95, 0x2D, 4};
    const uint8_t other[6] = {6, 0x2A, 0xDF, 0x95, 0x2D, 5};
    const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    uint8_t multicast[6] = {1, 0, 0x5E, 0, 0, 1};
    const uint32_t filter_before = ETH_MACPFR_HPF | ETH_MACPFR_HMC | ETH_MACPFR_PCF;
    s_rx_queue = (QueueHandle_t)(uintptr_t)1;
    CHECK(serial_net_input(&head) == 0 && queue_calls == 0);
    test_eth.MACPFR = filter_before;
    test_eth.MACA0HR = 0x12345678U; test_eth.MACA0LR = 0xABCDEF01U;
    test_eth.MACA1HR = UINT32_MAX;
    test_eth.MACA2HR = 0x87654321U; test_eth.MACA2LR = 0x10FEDCBAU;
    serial_net_attach(&netif);
    /* 独立断言bit4，避免测试仅重复待测宏；不能误开PR或覆盖其它MAC槽。 */
    CHECK(ETH_MACPFR_PM == 0x10U && test_eth.MACPFR == (filter_before | 0x10U));
    CHECK((test_eth.MACPFR & (ETH_MACPFR_PR | ETH_MACPFR_RA)) == 0);
    CHECK(test_eth.MACA1LR == 0x95DF2A06U && test_eth.MACA1HR == 0x8000042DU);
    CHECK(test_eth.MACA0HR == 0x12345678U && test_eth.MACA0LR == 0xABCDEF01U);
    CHECK(test_eth.MACA2HR == 0x87654321U && test_eth.MACA2LR == 0x10FEDCBAU);
    CHECK(netif.hwaddr[0] == 2 && memcmp(s_mac, ours, 6) == 0);

    for (unsigned i = 14; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)i;
    s_active = 1;
    receive_case(multicast, 0x0800, 0, 1);
    multicast[3] = 0x7F; multicast[4] = 255; multicast[5] = 255;
    receive_case(multicast, 0x0800, 0, 1);
    multicast[3] = 0x80;
    receive_case(multicast, 0x0800, 0, 0);
    multicast[3] = 0;
    receive_case(multicast, 0x0806, 0, 0);
    receive_case(multicast, 0x86DD, 0, 0);
    receive_case(multicast, 0x8100, 0, 0);
    multicast[0] = 0x33; multicast[1] = 0x33;
    receive_case(multicast, 0x0800, 0, 0);
    receive_case(multicast, 0x86DD, 0, 0);
    receive_case(other, 0x0800, 0, 0);
    receive_case(netif.hwaddr, 0x0800, 0, 0);
    receive_case(broadcast, 0x0800, 0, 1);
    receive_case(broadcast, 0x0806, 0, 1);
    receive_case(ours, 0x0800, 1, 1);
    receive_case(ours, 0x0806, 1, 1);
    receive_case(ours, 0x86DD, 1, 0);
    receive_case(ours, 0x8100, 1, 0);

    multicast[0] = 1; multicast[1] = 0;
    power_state = PWR_CTRL_STATE_OFF;
    receive_case(multicast, 0x0800, 0, 0);
    receive_case(ours, 0x0800, 1, 0);
    power_state = PWR_CTRL_STATE_ON; s_active = 0;
    receive_case(multicast, 0x0800, 0, 0);
    s_active = 1; queue_accept = 0;
    receive_case(multicast, 0x0800, 0, 1);
    CHECK(s_drops == 1);
    queue_accept = 1; head.tot_len = SN_ETH_MAX + 1U;
    receive_case(ours, 0x0800, 1, 0);
    CHECK(s_drops == 2);
    head.tot_len = 13;
    receive_case(ours, 0x0800, 0, 0);
    head.tot_len = 60; tail.len = 5;
    receive_case(ours, 0x0800, 0, 0);
    message("PASS: serial-net MCU MAC filters, IPv4 multicast, pbuf chains and ownership\n");
    finish(0);
}
