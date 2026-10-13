/* QEMU离线验证真实槽位读取代码；仅把GPIOB寄存器替换为内存，不访问板卡。 */
#include "stm32h7xx_hal.h"

static GPIO_TypeDef s_gpio_b;
#undef GPIOB
#define GPIOB (&s_gpio_b)
/* 直接编译生产实现，避免另写一份解码逻辑导致测试与实际代码脱节。 */
#include "../Core/Src/board_identity.c"

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

void test_abort(void)
{
    message("FAIL: board identity CPU exception\n");
    finish(1U);
}

void test_main(void)
{
    const uint32_t noise[] = {0U, 0xFFC7U, 0xAA87U, 0xFFFFFFFFU};
    for (uint32_t slot = 0U; slot < 8U; ++slot) {
        for (uint32_t index = 0U; index < sizeof(noise) / sizeof(noise[0]); ++index) {
            s_gpio_b.IDR = (slot << 3U) | (noise[index] & ~0x38U);
            const uint32_t before = s_gpio_b.IDR;
            if (board_slot_code() != slot || s_gpio_b.IDR != before) {
                message("FAIL: slot code, bit order or read-only boundary\n");
                finish(1U);
            }
        }
    }
    /* 全高是合法槽位7；随后全低必须立即读回0，不能沿用之前的缓存。 */
    s_gpio_b.IDR = 0U;
    if (board_slot_code() != 0U) test_abort();
    message("PASS: actual B1 slot reader, 32 pin patterns and live read\n");
    finish(0U);
}
