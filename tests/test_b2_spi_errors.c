/* 包含目标工程实际驱动，仅运行纯错误分类；硬件函数由链接器剔除，不访问寄存器。 */
#include "b2_spi.c"

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

void test_abort(void) { message("FAIL: B2 SPI error classifier\n"); finish(1U); }
#define CHECK(value) do { if (!(value)) test_abort(); } while (0)

void test_main(void)
{
    /* 穷举两条流的FE/TE/DME组合与直接/FIFO模式；完成及其他流标志不得误判。 */
    for (unsigned int fifo = 0U; fifo < 2U; ++fifo) {
        uint32_t fcr = fifo ? DMA_SxFCR_DMDIS : 0U;
        for (unsigned int flags = 0U; flags < 8U; ++flags) {
            uint32_t rx = DMA_LISR_TCIF3 | DMA_LISR_HTIF3 | DMA_LISR_TEIF0;
            uint32_t tx = DMA_HISR_TCIF4 | DMA_HISR_HTIF4 | DMA_HISR_TEIF7;
            if (flags & 1U) { rx |= DMA_LISR_FEIF3; tx |= DMA_HISR_FEIF4; }
            if (flags & 2U) { rx |= DMA_LISR_TEIF3; tx |= DMA_HISR_TEIF4; }
            if (flags & 4U) { rx |= DMA_LISR_DMEIF3; tx |= DMA_HISR_DMEIF4; }
            int expected = ((flags & 6U) != 0U) || (fifo && (flags & 1U));
            CHECK((transfer_errors(0U, rx, 0U, fcr, 0U) != 0U) == expected);
            CHECK((transfer_errors(0U, 0U, tx, 0U, fcr) != 0U) == expected);
            CHECK((transfer_errors(0U, rx, tx, fcr, fcr) != 0U) == expected);
        }
    }
    CHECK(transfer_errors(0U, DMA_LISR_FEIF3, DMA_HISR_FEIF4, 0U, 0U) == 0U);
    CHECK(transfer_errors(0U, DMA_LISR_FEIF3, DMA_HISR_FEIF4, DMA_SxFCR_DMDIS, 0U) != 0U);
    CHECK(transfer_errors(0U, DMA_LISR_FEIF3, DMA_HISR_FEIF4, 0U, DMA_SxFCR_DMDIS) != 0U);
    const uint32_t spi_errors[] = {SPI_SR_OVR, SPI_SR_MODF, SPI_SR_TIFRE};
    for (unsigned int i = 0U; i < sizeof(spi_errors) / sizeof(spi_errors[0]); ++i)
        CHECK(transfer_errors(spi_errors[i], 0U, DMA_HISR_FEIF4, 0U, 0U) != 0U);
    CHECK(transfer_errors(SPI_SR_RXP | SPI_SR_TXP, 0U, 0U, 0U, 0U) == 0U);
    message("PASS: actual SPI DMA error classifier / direct-FIFO modes / fatal errors retained\n");
    finish(0U);
}
