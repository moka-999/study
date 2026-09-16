/* 仅使用已有CMSIS寄存器定义，不将App HAL/RTOS/网络栈带入B。
 * 依据RM0433 Flash章节及W25Q256FV Rev.G表3；单线写/擦使用02h/20h和4字节地址模式。
 */
#include "boot_port.h"
#include "stm32h743xx.h"
#include <stddef.h>

#define RAM_CODE __attribute__((section(".ramfunc"), noinline))
#define NOR_SIZE UINT32_C(0x02000000)
#define FLASH_ERRORS (FLASH_SR_WRPERR | FLASH_SR_PGSERR | FLASH_SR_STRBERR | FLASH_SR_INCERR | \
    FLASH_SR_OPERR | FLASH_SR_RDPERR | FLASH_SR_RDSERR | FLASH_SR_SNECCERR | \
    FLASH_SR_DBECCERR | FLASH_SR_CRCRDERR)

volatile uint32_t boot_diag[8];
static uint32_t write_target;
static volatile uint32_t guarded_address, guarded_error;

/* 全部从复位HSI 64MHz进入；用DWT做超时，不依赖中断或SysTick。 */
uint32_t boot_millis(void) { return DWT->CYCCNT / 64000U; }
void boot_watchdog_feed(void) { IWDG1->KR = 0xAAAAU; }

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
    boot_diag[0] = UINT32_C(0x424F4F54);
    /* 软件重启必须用SYSRESETREQ，不支持从任意App环境直接call到B。 */
    if ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI ||
        (RCC->CR & RCC_CR_HSIDIV) != 0U || RCC->D1CFGR != 0U) return -1;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = UINT32_C(0xC5ACCE55);
    DWT->CYCCNT = 0U; DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    SCB->SHCSR |= SCB_SHCSR_BUSFAULTENA_Msk;
    RCC->CSR |= RCC_CSR_LSION;
    if (wait_bits(&RCC->CSR, RCC_CSR_LSIRDY, RCC_CSR_LSIRDY, 1000U) != 0) return -1;
    IWDG1->KR = 0xCCCCU; IWDG1->KR = 0x5555U;
    IWDG1->PR = 6U; IWDG1->RLR = 4095U; IWDG1->WINR = 4095U;
    if (wait_bits(&IWDG1->SR, 7U, 0U, 100U) != 0) return -1;
    boot_watchdog_feed();
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
     * 先清WEL，再检查SR3.ADS=1；不写非易失ADP/QE/保护位，也不在忙时复位芯片。
     */
    uint8_t status3;
    if (nor_command(0x04U, 0, 0U, NULL, NULL, 0U, 0) != 0 ||
        nor_command(0xB7U, 0, 0U, NULL, NULL, 0U, 0) != 0 ||
        nor_command(0x15U, 0, 0U, &status3, NULL, 1U, 0) != 0 ||
        (status3 & 1U) == 0U) return -1;
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
        boot_watchdog_feed();
    }
    return 0;
}

static int writable_nor(uint32_t address, uint32_t bytes)
{
#if defined(BOOT_RAM_AGENT)
    /* 仅单独链接到RAM的SWD维护工具可恢复备份/写稳定源；正式B不包含此权限。 */
    return bytes != 0U && address < 0x410000U && bytes <= 0x410000U - address;
#else
    if (bytes == 0U || address >= OTA_STABLE_BASE || bytes > OTA_STABLE_BASE - address) return 0;
    return (address < 0x2000U && bytes <= 0x2000U - address) ||
           (address >= BOOT_JOURNAL_BASE && address < 0x6000U && bytes <= 0x6000U - address) ||
           address >= OTA_DOWNLOAD_BASE;
#endif
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
        address += n; data += n; bytes -= n; boot_watchdog_feed();
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
    boot_watchdog_feed();
    return 0;
}

void boot_allow_target(uint32_t base)
{
    write_target = (base == OTA_A1_BASE || base == OTA_A2_BASE) ? base : 0U;
    boot_diag[4] = write_target;
}

/**
 * 同Bank擦写期间整条路径及常量均在AXI RAM；禁用中断，不调用Flash上的库函数。
 * 超过5秒仍忙时只能在RAM请求复位，不能返回B的Flash取指。无进展不喂狗。
 */
static RAM_CODE int flash_operation(uint32_t address, const uint32_t *data)
{
    uint32_t primask = __get_PRIMASK(); __disable_irq();
    int bank2 = address >= OTA_A2_BASE;
    volatile uint32_t *sr = bank2 ? &FLASH->SR2 : &FLASH->SR1;
    volatile uint32_t *cr = bank2 ? &FLASH->CR2 : &FLASH->CR1;
    volatile uint32_t *ccr = bank2 ? &FLASH->CCR2 : &FLASH->CCR1;
    volatile uint32_t *key = bank2 ? &FLASH->KEYR2 : &FLASH->KEYR1;
    if ((*sr & (FLASH_SR_BSY | FLASH_SR_QW)) != 0U) { __set_PRIMASK(primask); return -1; }
    if ((*cr & FLASH_CR_LOCK) != 0U) { *key = 0x45670123U; *key = 0xCDEF89ABU; }
    if ((*cr & FLASH_CR_LOCK) != 0U) { __set_PRIMASK(primask); return -1; }
    *ccr = FLASH_ERRORS | FLASH_SR_EOP;
    *cr &= ~(FLASH_CR_PG | FLASH_CR_SER | FLASH_CR_SNB | FLASH_CR_PSIZE | FLASH_CR_FW);
    if (data == NULL) {
        uint32_t sector = (address & 0xFFFFFU) / OTA_SECTOR_BYTES;
        *cr |= FLASH_CR_SER | FLASH_CR_PSIZE_1 | (sector << FLASH_CR_SNB_Pos);
        *cr |= FLASH_CR_START;
    } else {
        *cr |= FLASH_CR_PG;
        __DSB(); __ISB();
        volatile uint32_t *destination = (volatile uint32_t *)address;
        for (uint32_t i = 0; i < 8U; ++i) destination[i] = data[i];
        __DSB(); __ISB();
    }
    uint32_t start = DWT->CYCCNT;
    while ((*sr & (FLASH_SR_BSY | FLASH_SR_QW)) != 0U) {
        if ((uint32_t)(DWT->CYCCNT - start) > 5000U * 64000U) {
            SCB->AIRCR = (0x5FAUL << SCB_AIRCR_VECTKEY_Pos) | SCB_AIRCR_SYSRESETREQ_Msk;
            __DSB(); for (;;) { __NOP(); }
        }
    }
    int result = (*sr & FLASH_ERRORS) == 0U ? 0 : -1;
    *ccr = FLASH_ERRORS | FLASH_SR_EOP;
    *cr &= ~(FLASH_CR_PG | FLASH_CR_SER | FLASH_CR_SNB);
    *cr |= FLASH_CR_LOCK;
    __DSB(); __ISB(); __set_PRIMASK(primask);
    return result;
}

int boot_flash_erase(void *ctx, uint32_t address)
{
    (void)ctx;
    if (write_target == 0U || address < write_target || address - write_target >= OTA_IMAGE_LIMIT ||
        (address & (OTA_SECTOR_BYTES - 1U)) != 0U) return -1;
    int result = flash_operation(address, NULL);
    if (result == 0) boot_watchdog_feed();
    return result;
}

int boot_flash_program(void *ctx, uint32_t address, const uint8_t *word)
{
    (void)ctx;
    uintptr_t source = (uintptr_t)word;
    if (write_target == 0U || address < write_target || address - write_target > OTA_IMAGE_LIMIT - 32U ||
        ((address | source) & 31U) != 0U ||
        !((source >= 0x20000000U && source <= 0x2001FFE0U) ||
          (source >= 0x24000000U && source <= 0x2407FFE0U))) return -1;
    int result = flash_operation(address, (const uint32_t *)word);
    if (result == 0) boot_watchdog_feed();
    return result;
}

extern uint32_t boot_guarded_load(uint32_t address);
extern char boot_load_instruction[], boot_load_resume[];

/** 仅恢复受控LDR的精确ECC总线错误；其他异常停在维护，绝不跳过任意指令。 */
RAM_CODE void boot_fault(uint32_t *frame)
{
    uint32_t error = FLASH->SR1 | FLASH->SR2;
    if (guarded_address >= OTA_A1_BASE && guarded_address < 0x08200000U &&
        frame[6] == (uint32_t)boot_load_instruction &&
        (SCB->CFSR & SCB_CFSR_PRECISERR_Msk) != 0U && (error & FLASH_SR_DBECCERR) != 0U) {
        guarded_error = 1U; ++boot_diag[7];
        FLASH->CCR1 = FLASH_SR_DBECCERR | FLASH_SR_SNECCERR;
        FLASH->CCR2 = FLASH_SR_DBECCERR | FLASH_SR_SNECCERR;
        SCB->CFSR = SCB->CFSR; SCB->HFSR = SCB_HFSR_FORCED_Msk;
        frame[0] = 0U; frame[6] = (uint32_t)boot_load_resume;
        __DSB(); return;
    }
    boot_diag[1] = BOOT_MAINTENANCE; boot_diag[2] = 0xECCFFFFFU;
    for (;;) { IWDG1->KR = 0xAAAAU; }
}

int boot_flash_read(void *ctx, uint32_t address, uint8_t *data, uint32_t bytes)
{
    (void)ctx;
    if (data == NULL || address < OTA_A1_BASE || address >= 0x081E0000U ||
        bytes > 0x081E0000U - address) return -1;
    FLASH->CCR1 = FLASH_ERRORS; FLASH->CCR2 = FLASH_ERRORS;
    while (bytes != 0U) {
        uint32_t aligned = address & ~3U, shift = address & 3U;
        guarded_error = 0U; guarded_address = aligned; __DSB();
        uint32_t value = boot_guarded_load(aligned);
        guarded_address = 0U;
        if (guarded_error != 0U || ((FLASH->SR1 | FLASH->SR2) & FLASH_ERRORS) != 0U) return -1;
        uint32_t n = 4U - shift; if (n > bytes) n = bytes;
        for (uint32_t i = 0; i < n; ++i) data[i] = (uint8_t)(value >> (8U * (shift + i)));
        data += n; address += n; bytes -= n;
    }
    return 0;
}

_Noreturn void boot_maintenance(uint32_t error)
{
    boot_diag[1] = BOOT_MAINTENANCE; boot_diag[2] = error;
    /* 没有可安全启动的App时停留，SWD仍可恢复；不靠无休止重启掩盖错误。 */
    for (;;) boot_watchdog_feed();
}

extern _Noreturn void boot_jump_tail(uint32_t base);
_Noreturn void boot_jump(uint32_t base)
{
    boot_diag[6] = base;
    __disable_irq(); SysTick->CTRL = 0U; SysTick->LOAD = 0U; SysTick->VAL = 0U;
    for (uint32_t i = 0; i < 8U; ++i) { NVIC->ICER[i] = UINT32_MAX; NVIC->ICPR[i] = UINT32_MAX; }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    QUADSPI->CR = 0U;
    boot_watchdog_feed(); boot_jump_tail(base);
}
