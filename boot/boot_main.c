#include "boot_port.h"
#include "boot_fru.h"
#include "boot_vectors.h"
#include "board_storage_config.h"
#include "stm32h743xx.h"

/* [0]标识，[1]阶段：1进入/2 FRU有效/3跳转/0xE1器件/0xE2记录/0xE3向量错误；
 * [2]是否装W25，[3]JEDEC ID。仅供Boot现场诊断，不持久化，不传给PC。 */
volatile uint32_t boot_diag[4];

static _Noreturn void stop(uint32_t reason)
{
    boot_diag[1] = reason;
    for (;;) { __WFI(); }
}

void boot_main(void)
{
    boot_diag[0] = 0x424F4F54U;
    boot_diag[1] = 1U;
    boot_diag[2] = BOARD_W25_PRESENT;
#if BOARD_W25_PRESENT
    if (boot_port_init() != 0) stop(0xE1U);
    const uint8_t *mac = NULL;
#if defined(B1_RK_UART)
    /* 与B1的board_mac_from_uid保持一致；仅为空白FRU提供本板身份。 */
    uint8_t address[6] = {2U};
    uint64_t hash = UINT64_C(1469598103934665603);
    for (unsigned i = 0U; i < 12U; ++i) {
        hash ^= ((const volatile uint8_t *)UID_BASE)[i];
        hash *= UINT64_C(1099511628211);
    }
    for (unsigned i = 1U; i < 6U; ++i) address[i] = (uint8_t)(hash >> ((i - 1U) * 8U));
    mac = address;
#endif
    if (boot_fru_prepare(&boot_flash, mac) != 0) stop(0xE2U);
    boot_diag[1] = 2U;
#endif
    const volatile uint32_t *vectors = (const volatile uint32_t *)BOOT_APP_BASE;
    uint32_t stack = vectors[0], entry = vectors[1];
    if (!boot_app_vectors_valid(stack, entry)) stop(0xE3U);
    boot_diag[1] = 3U;
    boot_jump(stack, entry);
}
