/*
 * 直接运行正式pac1934.c换算函数；仅QEMU离线检查，不验证实际电流或I2C时序。
 * 配合现有bootloader/tests/qemu_start.s、qemu_test.ld构建，不能烧录到B1。
 */
#include "pac1934.h"

_Static_assert(PAC1934_RSENSE_B1_UOHM == 20000U,
               "B1 2134 requires 20000 uohm");

/* 复用现有QEMU测试的半主机退出约定：0为通过，非0为失败。 */
static _Noreturn void finish(uint32_t code)
{
    const uint32_t block[] = {0x20026U, code};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register const uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) { }
}

static void message(const char *text)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = text;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}

#define CHECK(condition) do { \
    if (!(condition)) { message("FAIL: " #condition "\n"); finish(1U); } \
} while (0)

void test_abort(void)
{
    message("FAIL: CPU exception\n");
    finish(1U);
}

void test_main(void)
{
    const uint32_t b1_rsense = PAC1934_RSENSE_B1_UOHM;

    /* 同一半量程原始值，20mΩ应为10mΩ电流/功率的一半；总线电压不变。 */
    CHECK(pac1934_current_ma(32768U, b1_rsense) == 2500U);
    CHECK(pac1934_current_ma(32768U, 10000U) == 5000U);
    CHECK(pac1934_power_mw(0x08000000U, b1_rsense) == 80000U);
    CHECK(pac1934_power_mw(0x08000000U, 10000U) == 160000U);
    CHECK(pac1934_vbus_mv(32768U) == 16000U);

    /* 零点、最大有效寄存器值和小信号截断仍使用生产驱动的整数换算。 */
    CHECK(pac1934_current_ma(0U, b1_rsense) == 0U);
    CHECK(pac1934_power_mw(0U, b1_rsense) == 0U);
    CHECK(pac1934_current_ma(65535U, b1_rsense) == 4999U);
    CHECK(pac1934_power_mw(0x0FFFFFFFU, b1_rsense) == 159999U);
    CHECK(pac1934_current_ma(14U, b1_rsense) == 1U);
    CHECK(pac1934_power_mw(1678U, b1_rsense) == 1U);

    /* 换算API对零电阻防止除零；不把错误输入当成B1板级默认值。 */
    CHECK(pac1934_current_ma(32768U, 0U) == 0U);
    CHECK(pac1934_power_mw(0x08000000U, 0U) == 0U);
    message("PASS: actual PAC1934 C conversion, B1 20mohm / generic 10mohm\n");
    finish(0U);
}
