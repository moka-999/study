/* 实际驱动故障注入：不连接板卡，不擦写真实Flash，不替代电气验收。 */
#include "main.h"
#include "cmsis_os2.h"
#include "mcp4725.h"
#include "pcf8574.h"
#include "qspi_hw.h"
#include "eth_custom_phy_interface.h"
#include <string.h>

static void message(const char *s)
{
    register unsigned r0 __asm("r0") = 4U;
    register const char *r1 __asm("r1") = s;
    __asm volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
}
static void finish(unsigned status)
{
    unsigned args[] = {0x20026U, status};
    register unsigned r0 __asm("r0") = 0x20U;
    register const unsigned *r1 __asm("r1") = args;
    __asm volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {}
}
#define STR1(x) #x
#define STR(x) STR1(x)
#define CHECK(x) do { if (!(x)) { message("FAIL " STR(__LINE__) ": " #x "\n"); finish(1U); } } while (0)
void test_abort(void) { message("FAIL: exception\n"); finish(1U); }
static uint32_t ticks;
uint32_t HAL_GetTick(void) { return ticks; }
void HAL_Delay(uint32_t ms) { ticks += ms; }
osStatus_t osDelay(uint32_t ms) { ticks += ms; return osOK; }
static osKernelState_t kernel_state = osKernelRunning;
osKernelState_t osKernelGetState(void) { return kernel_state; }

/* PCF模拟外部输入拉低，以及地址/数据阶段NACK。 */
static uint8_t pcf_latch = 0xFFU, pcf_external = 0xFDU, iic_index, iic_address, nack_at;
void iic_start(void) { iic_index = 0U; }
void iic_stop(void) {}
uint8_t iic_wait_ack(void) { return iic_index == nack_at; }
void iic_send_byte(uint8_t value)
{
    if (iic_index++ == 0U) iic_address = value;
    else if ((iic_address & 1U) == 0U) pcf_latch = value;
}
uint8_t iic_read_byte(uint8_t ack) { CHECK(ack == 0U); return pcf_latch & pcf_external; }

/* NOR模型保留一块数据，其余地址视为已擦除；400秒等待在虚拟时基中推进。 */
QSPI_HandleTypeDef hqspi;
static QSPI_CommandTypeDef nor;
static uint8_t flash[65536], sr2, sr3, wel, busy_forever, suspended;
static uint8_t ignore_wren, ignore_mutation, wrong_id, ignore_ads;
static uint32_t nor_calls, nor_fail_at, busy_begin, busy_duration, resets, programs, erases;
static uint8_t busy(void) { return busy_forever || (uint32_t)(ticks - busy_begin) < busy_duration; }
static void nor_reset_case(void)
{
    memset(flash, 0x5A, sizeof(flash));
    memset(&nor, 0, sizeof(nor));
    sr2 = W25Q_SR2_QE; sr3 = 1U;
    wel = busy_forever = suspended = ignore_wren = ignore_mutation = wrong_id = ignore_ads = 0U;
    nor_calls = nor_fail_at = busy_duration = resets = programs = erases = 0U;
    ticks = busy_begin = 0U;
    kernel_state = osKernelRunning;
}
HAL_StatusTypeDef HAL_QSPI_Command(QSPI_HandleTypeDef *h, QSPI_CommandTypeDef *c, uint32_t timeout)
{
    (void)h; (void)timeout;
    if (++nor_calls == nor_fail_at) return HAL_ERROR;
    nor = *c;
    if (c->Instruction == 0x66U || c->Instruction == 0x99U) ++resets;
    if (c->Instruction == 0xB7U && !ignore_ads) sr3 = 1U;
    if (c->Instruction == 0x04U) wel = 0U;
    if (c->Instruction == 0x06U && !ignore_wren && !busy() && !suspended) wel = 1U;
    if (c->Instruction == W25Q_CMD_QUAD_READ) {
        CHECK(c->AlternateByteMode == QSPI_ALTERNATE_BYTES_4_LINES);
        CHECK(c->AlternateBytesSize == QSPI_ALTERNATE_BYTES_8_BITS && c->AlternateBytes == 0xFFU);
        CHECK(c->DummyCycles == 4U && c->AddressSize == QSPI_ADDRESS_32_BITS);
    }
    uint32_t size = c->Instruction == W25Q_CMD_SECTOR_ERASE ? W25Q_SECTOR_SIZE :
                    c->Instruction == W25Q_CMD_BLOCK_ERASE32 ? W25Q_BLOCK32_SIZE :
                    c->Instruction == W25Q_CMD_BLOCK_ERASE64 ? W25Q_BLOCK64_SIZE :
                    c->Instruction == W25Q_CMD_CHIP_ERASE ? W25Q_CAPACITY : 0U;
    if (size != 0U) {
        CHECK(wel && !busy() && !suspended);
        if (size != W25Q_CAPACITY) CHECK(c->AddressSize == QSPI_ADDRESS_32_BITS);
        ++erases; wel = 0U;
        if (!ignore_mutation) {
            uint32_t offset = c->Address % sizeof(flash);
            uint32_t count = size > sizeof(flash) ? sizeof(flash) : size;
            CHECK(offset + count <= sizeof(flash));
            memset(flash + offset, 0xFF, count);
            busy_begin = ticks;
            busy_duration = size == W25Q_CAPACITY ? 400000U : 2U;
        }
    }
    return HAL_OK;
}
HAL_StatusTypeDef HAL_QSPI_Transmit(QSPI_HandleTypeDef *h, uint8_t *data, uint32_t timeout)
{
    (void)h; (void)timeout;
    if (++nor_calls == nor_fail_at) return HAL_ERROR;
    CHECK(wel && !busy() && !suspended);
    wel = 0U;
    if (nor.Instruction == W25Q_CMD_WRITE_STATUS2) {
        if (!ignore_mutation) sr2 = *data;
    } else {
        CHECK(nor.Instruction == W25Q_CMD_QUAD_WRITE && nor.AddressSize == QSPI_ADDRESS_32_BITS);
        ++programs;
        if (!ignore_mutation)
            for (uint32_t i = 0U; i < nor.NbData; ++i) flash[(nor.Address + i) % sizeof(flash)] &= data[i];
    }
    busy_begin = ticks; busy_duration = 2U;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_QSPI_Receive(QSPI_HandleTypeDef *h, uint8_t *data, uint32_t timeout)
{
    (void)h; (void)timeout;
    if (++nor_calls == nor_fail_at) return HAL_ERROR;
    if (nor.Instruction == W25Q_CMD_READ_STATUS1) *data = (busy() ? 1U : 0U) | (wel ? 2U : 0U);
    else if (nor.Instruction == W25Q_CMD_READ_STATUS2) *data = sr2 | (suspended ? W25Q_SR2_SUS : 0U);
    else if (nor.Instruction == W25Q_CMD_READ_STATUS3) *data = sr3;
    else if (nor.Instruction == W25Q_CMD_READ_JEDEC_ID) {
        data[0] = wrong_id ? 0xFFU : 0xEFU; data[1] = 0x40U; data[2] = 0x19U;
    } else {
        CHECK(nor.Instruction == W25Q_CMD_QUAD_READ && !busy() && !suspended);
        for (uint32_t i = 0U; i < nor.NbData; ++i)
            data[i] = flash[(nor.Address + i) % sizeof(flash)];
    }
    return HAL_OK;
}

static void test_nor(void)
{
    nor_reset_case(); sr2 = 0U; sr3 = 0U;
    CHECK(qspi_init() == IF_OK && sr2 == W25Q_SR2_QE && sr3 == 1U && resets == 0U);
    uint32_t count = nor_calls;
    for (uint32_t i = 1U; i <= count; ++i) {
        nor_reset_case(); sr2 = sr3 = 0U; nor_fail_at = i;
        CHECK(qspi_init() != IF_OK && resets == 0U);
    }
    nor_reset_case(); busy_forever = 1U;
    CHECK(qspi_init() != IF_OK && resets == 0U && programs == 0U && ticks >= 500U);
    nor_reset_case(); suspended = 1U;
    CHECK(qspi_init() != IF_OK && resets == 0U);
    nor_reset_case(); wrong_id = 1U; CHECK(qspi_init() != IF_OK);
    nor_reset_case(); sr3 = 0U; ignore_ads = 1U; CHECK(qspi_init() != IF_OK);
    nor_reset_case(); sr2 = 0U; ignore_mutation = 1U; CHECK(qspi_init() != IF_OK);

    uint8_t zero[256] = {0}, value[256];
    nor_reset_case(); CHECK(qspi_write_page(0U, zero, sizeof(zero)) == IF_OK);
    count = nor_calls;
    CHECK(qspi_read_data(0U, value, sizeof(value)) == IF_OK && memcmp(value, zero, sizeof(value)) == 0);
    for (uint32_t i = 1U; i <= count; ++i) {
        nor_reset_case(); nor_fail_at = i;
        CHECK(qspi_write_page(0U, zero, sizeof(zero)) != IF_OK);
    }
    nor_reset_case(); ignore_wren = 1U;
    CHECK(qspi_write_page(0U, zero, 1U) != IF_OK && programs == 0U);
    CHECK(qspi_erase_sector(0U) != IF_OK && erases == 0U);
    nor_reset_case(); ignore_mutation = 1U;
    CHECK(qspi_write_page(0U, zero, 1U) != IF_OK);
    CHECK(qspi_erase_sector(0U) != IF_OK && flash[0] == 0x5AU);
    nor_reset_case(); suspended = 1U;
    CHECK(qspi_write_page(0U, zero, 1U) != IF_OK && programs == 0U);
    CHECK(qspi_read_data(0U, value, 1U) != IF_OK);
    nor_reset_case(); CHECK(qspi_erase_sector(0U) == IF_OK && flash[4095] == 0xFFU && flash[4096] == 0x5AU);
    nor_reset_case(); CHECK(qspi_erase_block32(0U) == IF_OK && flash[32767] == 0xFFU && flash[32768] == 0x5AU);
    nor_reset_case(); CHECK(qspi_erase_block64(0U) == IF_OK && flash[65535] == 0xFFU);
    nor_reset_case(); CHECK(qspi_erase_chip() == IF_OK && ticks >= 400000U);
    nor_reset_case(); ticks = UINT32_MAX - 4U; busy_forever = 1U; kernel_state = osKernelReady;
    CHECK(qspi_wait_busy(10U) == IF_TIMEOUT && ticks == 5U);
    nor_reset_case(); CHECK(qspi_wait_busy(0U) == IF_OK);
    CHECK(qspi_write_page(255U, zero, 2U) == IF_ERR && nor_calls == 2U);
    CHECK(qspi_erase_sector(1U) == IF_ERR);
    CHECK(qspi_read_data(W25Q_CAPACITY - 1U, value, 2U) == IF_ERR);
    CHECK(qspi_write_page(W25Q_CAPACITY - 256U, zero, 256U) == IF_OK);
    message("PASS NOR: init/write fault sweep, WEL/protection/readback, mode byte, 400s erase and tick wrap\n");
}

/* PHY仅模拟MDIO返回值，模式解码运行正式函数。 */
static uint32_t phy_regs[32], phy_reads, phy_writes, phy_fail_at;
static int32_t phy_init_result;
static int32_t phy_init(void) { return phy_init_result; }
static int32_t phy_read(uint32_t addr, uint32_t reg, uint32_t *value)
{
    if (++phy_reads == phy_fail_at) return -1;
    *value = addr == 5U ? phy_regs[reg] : 0xFFFFU;
    return 0;
}
static int32_t phy_write(uint32_t addr, uint32_t reg, uint32_t value)
{ CHECK(addr == 5U); ++phy_writes; phy_regs[reg] = value; return 0; }
static int32_t phy_tick(void) { return (int32_t)ticks; }
user_phy_Object_t UserPhyObj;
static void phy_reset_case(void)
{
    memset(phy_regs, 0, sizeof(phy_regs));
    memset(&UserPhyObj, 0, sizeof(UserPhyObj));
    phy_reads = phy_writes = phy_fail_at = 0U; phy_init_result = 0;
    phy_regs[USER_PHY_PHYI2R] = 0x0128U;
    user_phy_IOCtx_t io = {.Init=phy_init, .ReadReg=phy_read, .WriteReg=phy_write, .GetTick=phy_tick};
    CHECK(USER_PHY_RegisterBusIO(&UserPhyObj, &io) == USER_PHY_STATUS_OK);
}

/* 生产MAC更新函数以轻量netif和HAL替身执行；不模拟真实DMA/中断并发。 */
struct netif { int link; };
static void netif_set_link_down(struct netif *n) { n->link = 0; }
static void netif_set_link_up(struct netif *n) { n->link = 1; }
ETH_HandleTypeDef heth;
static osMutexId_t eth_rx_mutex;
static int locked, fail_mac;
static unsigned mac_sets, mac_starts, mac_stops;
static ETH_MACConfigTypeDef applied_mac;
osStatus_t osMutexAcquire(osMutexId_t id, uint32_t timeout)
{ (void)id; (void)timeout; CHECK(!locked); locked = 1; return osOK; }
osStatus_t osMutexRelease(osMutexId_t id)
{ (void)id; CHECK(locked); locked = 0; return osOK; }
HAL_StatusTypeDef HAL_ETH_GetMACConfig(const ETH_HandleTypeDef *h, ETH_MACConfigTypeDef *c)
{ (void)h; CHECK(locked); memset(c, 0, sizeof(*c)); return HAL_OK; }
HAL_StatusTypeDef HAL_ETH_SetMACConfig(ETH_HandleTypeDef *h, ETH_MACConfigTypeDef *c)
{ CHECK(locked && h->gState == HAL_ETH_STATE_READY); ++mac_sets; applied_mac = *c; return fail_mac ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_ETH_Start_IT(ETH_HandleTypeDef *h)
{ CHECK(locked && h->gState == HAL_ETH_STATE_READY); ++mac_starts; h->gState = HAL_ETH_STATE_STARTED; return HAL_OK; }
HAL_StatusTypeDef HAL_ETH_Stop_IT(ETH_HandleTypeDef *h)
{ CHECK(locked && h->gState == HAL_ETH_STATE_STARTED); ++mac_stops; h->gState = HAL_ETH_STATE_READY; return HAL_OK; }
#include "mac_link_under_test.h"

static void test_phy_and_mac(void)
{
    phy_reset_case(); CHECK(USER_PHY_RegisterBusIO(&UserPhyObj, NULL) == USER_PHY_STATUS_ERROR);
    phy_init_result = -1; CHECK(USER_PHY_Init(&UserPhyObj) == USER_PHY_STATUS_ERROR);
    phy_reset_case(); phy_regs[USER_PHY_PHYI2R] = 0U;
    CHECK(USER_PHY_Init(&UserPhyObj) == USER_PHY_STATUS_ADDRESS_ERROR && !UserPhyObj.Is_Initialized);
    phy_reset_case(); CHECK(USER_PHY_Init(&UserPhyObj) == USER_PHY_STATUS_OK && UserPhyObj.DevAddr == 5U);
    phy_regs[USER_PHY_BSR] = USER_PHY_BSR_LINK_STATUS;
    static const uint16_t resolved[] = {0x6C00U, 0x4C00U, 0x2C00U, 0x0C00U};
    struct netif netif = {0}; heth.gState = HAL_ETH_STATE_READY;
    for (unsigned i = 0U; i < 4U; ++i) {
        phy_regs[USER_PHY_SPECIFIC_STATUS] = resolved[i];
        CHECK(USER_PHY_GetLinkState(&UserPhyObj) == (int32_t)(2U + i));
        ethernet_link_update(&netif);
        CHECK(netif.link && !locked && mac_sets == i + 1U && mac_starts == i + 1U);
        CHECK(applied_mac.Speed == (i < 2U ? ETH_SPEED_100M : ETH_SPEED_10M));
        CHECK(applied_mac.DuplexMode == ((i & 1U) ? ETH_HALFDUPLEX_MODE : ETH_FULLDUPLEX_MODE));
        ethernet_link_update(&netif); CHECK(mac_sets == i + 1U);
    }
    CHECK(mac_stops == 3U);
    phy_regs[USER_PHY_SPECIFIC_STATUS] = 0x0400U;
    CHECK(USER_PHY_GetLinkState(&UserPhyObj) == USER_PHY_STATUS_AUTONEGO_NOTDONE);
    ethernet_link_update(&netif); CHECK(!netif.link && heth.gState == HAL_ETH_STATE_READY);
    phy_regs[USER_PHY_SPECIFIC_STATUS] = 0x8C00U;
    CHECK(USER_PHY_GetLinkState(&UserPhyObj) == USER_PHY_STATUS_ERROR);
    phy_regs[USER_PHY_SPECIFIC_STATUS] = 0x6C00U; fail_mac = 1;
    ethernet_link_update(&netif); CHECK(!netif.link && heth.gState == HAL_ETH_STATE_READY);
    fail_mac = 0; ethernet_link_update(&netif); CHECK(netif.link);
    phy_fail_at = phy_reads + 1U;
    ethernet_link_update(&netif); CHECK(!netif.link);
    for (uint32_t i = 1U; i <= 3U; ++i) {
        phy_fail_at = phy_reads + i;
        CHECK(USER_PHY_GetLinkState(&UserPhyObj) == USER_PHY_STATUS_READ_ERROR);
    }
    phy_fail_at = phy_reads + 1U;
    CHECK(USER_PHY_SetLinkState(&UserPhyObj, USER_PHY_STATUS_100MBITS_FULLDUPLEX) == USER_PHY_STATUS_READ_ERROR);
    CHECK(phy_writes == 0U);
    CHECK(USER_PHY_SetLinkState(&UserPhyObj, 99U) == USER_PHY_STATUS_ERROR && phy_writes == 0U);
    phy_fail_at = 0U; phy_regs[USER_PHY_BCR] = USER_PHY_BCR_AUTONEGO_EN;
    CHECK(USER_PHY_SetLinkState(&UserPhyObj, USER_PHY_STATUS_100MBITS_HALFDUPLEX) == USER_PHY_STATUS_OK);
    CHECK(phy_regs[USER_PHY_BCR] == USER_PHY_BCR_SPEED_SELECT);
    message("PASS PHY/MAC: full ID, actual resolved modes, down/retry, invalid state and MDIO errors\n");
}

static void test_dac_and_pcf(void)
{
    uint16_t code = 123U;
    CHECK(mcp4725_mv_to_code(1500U, 3300U, &code) == IF_OK && code == 1862U);
    CHECK(mcp4725_mv_to_code(3300U, 3300U, &code) == IF_OK && code == 4095U);
    CHECK(mcp4725_mv_to_code(0U, 3300U, &code) == IF_OK && code == 0U);
    CHECK(mcp4725_mv_to_code(3301U, 3300U, &code) == IF_ERR && code == 0U);
    CHECK(mcp4725_mv_to_code(1U, 0U, &code) == IF_ERR);
    CHECK(pcf8574_write_byte(0xFFU) == 0 && pcf8574_read_bit(1U) == 0);
    CHECK(pcf8574_write_bit(7U, 0U) == 0 && pcf_latch == 0x7FU);
    pcf_external = 0xFFU;
    CHECK(pcf8574_read_bit(1U) == 1);
    CHECK(pcf8574_write_bit(1U, 0U) == -1 && pcf8574_write_bit(8U, 0U) == -1);
    for (uint8_t i = 1U; i <= 2U; ++i) {
        nack_at = i;
        CHECK(pcf8574_write_byte(0xFFU) == -1);
        nack_at = 0U;
        CHECK(pcf8574_write_bit(7U, 1U) == -1);
        CHECK(pcf8574_write_byte(0xFFU) == 0);
    }
    nack_at = 1U; CHECK(pcf8574_read_bit(7U) == -1);
    CHECK(pcf8574_read_bit(8U) == -1);
    message("PASS DAC/PCF: 4096 scale, clamp, input release, latch shadow and NACK\n");
}
void test_main(void)
{
    test_nor(); test_phy_and_mac(); test_dac_and_pcf();
    message("PASS: driver reliability; no hardware accessed\n");
    finish(0U);
}
