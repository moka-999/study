#include "boot_port.h"
#include "boot_fru.h"
#include "boot_vectors.h"
#include "board_storage_config.h"
#include "stm32h743xx.h"
#include <string.h>
#define NOR_SIZE 0x02000000U

static int wait_bits(volatile uint32_t *reg, uint32_t mask, uint32_t value, uint32_t ms)
{
    uint32_t start = DWT->CYCCNT;
    while ((*reg & mask) != value) {
        if ((uint32_t)(DWT->CYCCNT - start) >= ms * 64000U) return -1;
    }
    return 0;
}

static void gpio_af(GPIO_TypeDef *gpio, uint32_t pin, uint32_t af)
{
    gpio->AFR[pin / 8U] = (gpio->AFR[pin / 8U] & ~(15UL << ((pin % 8U) * 4U))) |
                          (af << ((pin % 8U) * 4U));
    gpio->OTYPER &= ~(1UL << pin);
    gpio->OSPEEDR |= 3UL << (pin * 2U);
    gpio->PUPDR = (gpio->PUPDR & ~(3UL << (pin * 2U))) | (1UL << (pin * 2U));
    gpio->MODER = (gpio->MODER & ~(3UL << (pin * 2U))) | (2UL << (pin * 2U));
}

/** 一次间接QSPI事务。所有等待有界；数据逐字节读写FIFO，不需要DMA/Cache。 */
static int nor_command(uint32_t opcode, int has_address, uint32_t address,
                       uint8_t *rx, const uint8_t *tx, uint32_t bytes, int quad_opcode)
{
    if (wait_bits(&QUADSPI->SR, QUADSPI_SR_BUSY, 0U, 100U) != 0) return -1;
    QUADSPI->FCR = 0x1BU;
    QUADSPI->DLR = bytes != 0U ? bytes - 1U : 0U;
    uint32_t ccr = opcode | (quad_opcode ? QUADSPI_CCR_IMODE : QUADSPI_CCR_IMODE_0);
    if (has_address) ccr |= QUADSPI_CCR_ADMODE_0 | QUADSPI_CCR_ADSIZE;
    if (bytes != 0U) ccr |= QUADSPI_CCR_DMODE_0;
    if (rx != NULL) ccr |= QUADSPI_CCR_FMODE_0;
    QUADSPI->CCR = ccr;
    if (has_address) QUADSPI->AR = address;
    uint32_t start = DWT->CYCCNT;
    for (uint32_t i = 0; i < bytes; ++i) {
        for (;;) {
            uint32_t status = QUADSPI->SR;
            if ((status & QUADSPI_SR_TEF) != 0U ||
                (uint32_t)(DWT->CYCCNT - start) > 500U * 64000U) goto failed;
            uint32_t level = (status & QUADSPI_SR_FLEVEL) >> QUADSPI_SR_FLEVEL_Pos;
            if (rx != NULL ? level != 0U : level < 32U) break;
        }
        if (rx != NULL) rx[i] = *(volatile uint8_t *)&QUADSPI->DR;
        else *(volatile uint8_t *)&QUADSPI->DR = tx[i];
    }
    if (wait_bits(&QUADSPI->SR, QUADSPI_SR_TCF, QUADSPI_SR_TCF, 100U) != 0 ||
        (QUADSPI->SR & QUADSPI_SR_TEF) != 0U) goto failed;
    QUADSPI->FCR = QUADSPI_FCR_CTCF;
    return 0;
failed:
    QUADSPI->CR |= QUADSPI_CR_ABORT;
    (void)wait_bits(&QUADSPI->CR, QUADSPI_CR_ABORT, 0U, 100U);
    return -1;
}

static int nor_ready(uint32_t timeout_ms)
{
    uint32_t start = DWT->CYCCNT;
    uint8_t status;
    do {
        if (nor_command(0x05U, 0, 0U, &status, NULL, 1U, 0) != 0) return -1;
        if ((status & 1U) == 0U) return 0;
    } while ((uint32_t)(DWT->CYCCNT - start) < timeout_ms * 64000U);
    return -1;
}

static int nor_write_enable(void)
{
    uint8_t status;
    if (nor_ready(5000U) != 0 || nor_command(0x06U, 0, 0U, NULL, NULL, 0U, 0) != 0 ||
        nor_command(0x05U, 0, 0U, &status, NULL, 1U, 0) != 0) return -1;
    return (status & 2U) != 0U ? 0 : -1;
}

int boot_port_init(void)
{
    /* 软件重启必须用SYSRESETREQ，不支持从任意App环境直接call到B。 */
    if ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI ||
        (RCC->CR & RCC_CR_HSIDIV) != 0U || RCC->D1CFGR != 0U) return -1;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = UINT32_C(0xC5ACCE55);
    DWT->CYCCNT = 0U; DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOBEN | RCC_AHB4ENR_GPIOFEN;
    RCC->AHB3ENR |= RCC_AHB3ENR_QSPIEN;
    (void)RCC->AHB3ENR;
    RCC->D1CCIPR &= ~RCC_D1CCIPR_QSPISEL; /* D1HCLK=64MHz，QSPI再除8。 */
    RCC->AHB3RSTR |= RCC_AHB3RSTR_QSPIRST; RCC->AHB3RSTR &= ~RCC_AHB3RSTR_QSPIRST;
    /* V2与B1的MCU W25均为PB2/PB6、PF6..PF9 BK1；B1不是RK侧FSPI。
     * B一直用复位HSI，不依赖B1的25MHz或V2的外部晶振。 */
    gpio_af(GPIOB, 2U, 9U); gpio_af(GPIOB, 6U, 10U);
    gpio_af(GPIOF, 6U, 9U); gpio_af(GPIOF, 7U, 9U);
    gpio_af(GPIOF, 8U, 10U); gpio_af(GPIOF, 9U, 10U);
    QUADSPI->DCR = (24U << QUADSPI_DCR_FSIZE_Pos) | (2U << QUADSPI_DCR_CSHT_Pos) | QUADSPI_DCR_CKMODE;
    QUADSPI->CR = (7U << QUADSPI_CR_PRESCALER_Pos) | QUADSPI_CR_EN;
    /* 退出旧QPI；随后WP/HOLD保持高。禁止在BUSY或SUSPEND时重置NOR。 */
    if (nor_command(0xFFU, 0, 0U, NULL, NULL, 0U, 1) != 0) return -1;
    GPIOF->BSRR = (1U << 6U) | (1U << 7U);
    GPIOF->MODER = (GPIOF->MODER & ~((3U << 12U) | (3U << 14U))) | (1U << 12U) | (1U << 14U);
    uint8_t id[3], status2;
    if (nor_ready(5000U) != 0 || nor_command(0x35U, 0, 0U, &status2, NULL, 1U, 0) != 0 ||
        (status2 & 0x80U) != 0U || nor_command(0x9FU, 0, 0U, id, NULL, 3U, 0) != 0) return -1;
    boot_diag[3] = ((uint32_t)id[0] << 16U) | ((uint32_t)id[1] << 8U) | id[2];
    if (boot_diag[3] != UINT32_C(0xEF4019)) return -1;
    /*
     * FV与JV的JEDEC同为EF4019，不能据此使用JV新增的12h/21h写擦指令。
     * FV手册P27/P38规定：B7h进入易失的4字节地址模式，再使用02h/20h。
     * 先清WEL，再检查SR3.ADS=1；不写非易失ADP/保护位，QE由下方单独设置，也不在忙时复位芯片。
     */
    uint8_t status3;
    if (nor_command(0x04U, 0, 0U, NULL, NULL, 0U, 0) != 0 ||
        nor_command(0xB7U, 0, 0U, NULL, NULL, 0U, 0) != 0 ||
        nor_command(0x15U, 0, 0U, &status3, NULL, 1U, 0) != 0 ||
        (status3 & 1U) == 0U) return -1;
    /* QE只在缺失时设置并回读；保留其余SR2位，不改保护范围。 */
    if ((status2 & 2U) == 0U) {
        uint8_t wanted = status2 | 2U;
        if (nor_write_enable() != 0 ||
            nor_command(0x31U, 0, 0U, NULL, &wanted, 1U, 0) != 0 ||
            nor_ready(100U) != 0 ||
            nor_command(0x35U, 0, 0U, &status2, NULL, 1U, 0) != 0 || status2 != wanted) return -1;
    }
    return 0;
}

int boot_nor_read(void *ctx, uint32_t address, uint8_t *data, uint32_t bytes)
{
    (void)ctx;
    if (data == NULL || bytes == 0U || address >= NOR_SIZE || bytes > NOR_SIZE - address) return -1;
    /* 有界分块也便于备份较大区域，不让单次FIFO事务超时。 */
    while (bytes != 0U) {
        uint32_t n = bytes < 1024U ? bytes : 1024U;
        if (nor_command(0x13U, 1, address, data, NULL, n, 0) != 0) return -1;
        data += n; address += n; bytes -= n;
        
    }
    return 0;
}

static int writable_nor(uint32_t address, uint32_t bytes)
{
    return bytes != 0U && address >= FLASH_LAYOUT_FRU_SLOT_A &&
           address < FLASH_LAYOUT_FRU_SLOT_B + 4096U &&
           bytes <= FLASH_LAYOUT_FRU_SLOT_B + 4096U - address;
}

int boot_nor_program(void *ctx, uint32_t address, const uint8_t *data, uint32_t bytes)
{
    (void)ctx;
    uint8_t verify[256];
    if (data == NULL || !writable_nor(address, bytes)) return -1;
    while (bytes != 0U) {
        uint32_t n = 256U - (address & 255U);
        if (n > bytes) n = bytes;
        if (nor_write_enable() != 0 || nor_command(0x02U, 1, address, NULL, data, n, 0) != 0 ||
            nor_ready(20U) != 0 || boot_nor_read(NULL, address, verify, n) != 0) return -1;
        for (uint32_t i = 0; i < n; ++i) if (verify[i] != data[i]) return -1;
        address += n; data += n; bytes -= n; 
    }
    return 0;
}

int boot_nor_erase(void *ctx, uint32_t address)
{
    (void)ctx;
    if ((address & 4095U) != 0U || !writable_nor(address, 4096U)) return -1;
    if (nor_write_enable() != 0 || nor_command(0x20U, 1, address, NULL, NULL, 0U, 0) != 0 ||
        nor_ready(2000U) != 0) return -1;
    /* BUSY=0不等于指令被芯片接受，必须检查整扇区确实为FF。 */
    uint8_t verify[256];
    for (uint32_t offset = 0U; offset < 4096U; offset += sizeof(verify)) {
        if (boot_nor_read(NULL, address + offset, verify, sizeof(verify)) != 0) return -1;
        for (uint32_t i = 0U; i < sizeof(verify); ++i) if (verify[i] != 0xFFU) return -1;
    }
    
    return 0;
}

static int read_fru(flash_if_t *flash, uint32_t address, uint8_t *data, uint32_t size)
{
    (void)flash;
    return boot_nor_read(NULL, address, data, size);
}

static int write_fru(flash_if_t *flash, uint32_t address, const uint8_t *data, uint32_t size)
{
    (void)flash;
    static uint8_t sector[4096];
    /* 记录层一次只写一个FRU扇区；先读整扇区，保留记录之外的数据。 */
    if (data == NULL || (address != FLASH_LAYOUT_FRU_SLOT_A && address != FLASH_LAYOUT_FRU_SLOT_B) ||
        size == 0U || size > sizeof(sector)) return -1;
    if (boot_nor_read(NULL, address, sector, sizeof(sector)) != 0) return -1;
    if (memcmp(sector, data, size) == 0) return 0;
    memcpy(sector, data, size);
    if (boot_nor_erase(NULL, address) != 0) return -1;
    for (unsigned offset = 0U; offset < sizeof(sector); offset += 256U) {
        int erased = 1;
        for (unsigned i = 0U; i < 256U; ++i) if (sector[offset + i] != 0xFFU) erased = 0;
        if (!erased && boot_nor_program(NULL, address + offset, sector + offset, 256U) != 0) return -1;
    }
    return 0;
}

flash_if_t boot_flash = {.read = read_fru, .write = write_fru};

_Noreturn void boot_jump(uint32_t stack, uint32_t entry)
{
    __disable_irq();
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;
    for (unsigned i = 0U; i < 8U; ++i) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
#if BOARD_W25_PRESENT
    QUADSPI->CR = 0U;
#endif
    SCB->VTOR = BOOT_APP_BASE;
    __DSB(); __ISB();
    /* MSP切换后不可再使用C栈；所有寄存器参数在此之前已求值。 */
    __asm__ volatile("movs r2, #0\n"
                     "msr control, r2\nmsr basepri, r2\nmsr faultmask, r2\n"
                     "msr msp, %0\ndsb\nisb\ncpsie i\nbx %1\n"
                     : : "r"(stack), "r"(entry) : "r2", "memory");
    __builtin_unreachable();
}
