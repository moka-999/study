#ifndef BOOT_VECTORS_H
#define BOOT_VECTORS_H
#include <stdint.h>
#define BOOT_APP_BASE UINT32_C(0x08020000)
/* 返回1表示栈在DTCM且8字节对齐、Thumb入口在App区；不代表镜像CRC认证。 */
static inline int boot_app_vectors_valid(uint32_t stack, uint32_t entry)
{
    return stack > 0x20000000U && stack <= 0x20020000U && (stack & 7U) == 0U &&
           (entry & 1U) != 0U && (entry & ~1U) >= BOOT_APP_BASE + 664U && entry < 0x08200000U;
}
#endif
