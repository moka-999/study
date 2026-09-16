/* 仅供授权后的SWD备份/恢复/部署，整个ELF装入RAM，不是可烧录到B的产品程序。 */
#include "boot_port.h"
#include "stm32h743xx.h"
#include <string.h>

/* [0]就绪魔数；[1]命令；[2]地址；[3]长度；[4]结果；[5]本次完成计数；[6]写授权钥匙。 */
volatile uint32_t agent_mailbox[8];
_Alignas(32) uint8_t agent_buffer[65536];
static const boot_store_t store = {NULL, boot_nor_read, boot_nor_program, boot_nor_erase};

/** GDB在此设置硬件断点；只在初始化/单次命令结束调用，便于同步验证结果。 */
__attribute__((noinline)) void agent_checkpoint(void) { __asm__ volatile("nop"); }

void boot_main(void)
{
    DBGMCU->APB4FZ1 |= DBGMCU_APB4FZ1_DBG_IWDG1; /* 只在此调试工具中暂停看门狗计时。 */
    if (boot_port_init() != 0) boot_maintenance(0xA001U);
    agent_mailbox[0] = 0x4147454EU;
    agent_checkpoint();
    for (;;) {
        boot_watchdog_feed();
        uint32_t command = agent_mailbox[1];
        if (command == 0U) continue;
        uint32_t address = agent_mailbox[2], bytes = agent_mailbox[3];
        int result = -1;
        if (command == 1U && bytes > 0U && bytes <= sizeof(agent_buffer)) {
            result = boot_nor_read(NULL, address, agent_buffer, bytes);
        } else if (agent_mailbox[6] == 0x53574457U) {
            if (command == 2U && bytes > 0U && bytes <= sizeof(agent_buffer)) {
                result = boot_nor_program(NULL, address, agent_buffer, bytes);
            } else if (command == 3U) {
                result = boot_nor_erase(NULL, address);
            } else if (command == 4U && bytes == sizeof(ota_image_t)) {
                boot_record_t initial = {0};
                ota_image_t image;
                memcpy(&image, agent_buffer, sizeof(image));
                initial.active = image.slot_base == OTA_A1_BASE ? 0U : 1U;
                initial.previous = BOOT_SLOT_NONE; initial.valid_mask = 1U << initial.active;
                initial.images[initial.active] = image;
                result = boot_record_save(&store, &initial);
            } else if (command == 5U && bytes == sizeof(boot_record_t)) {
                boot_record_t record;
                memcpy(&record, agent_buffer, sizeof(record));
                result = boot_record_save(&store, &record);
            }
        }
        agent_mailbox[4] = (uint32_t)result;
        agent_mailbox[6] = 0U;
        ++agent_mailbox[5]; __DMB(); agent_mailbox[1] = 0U;
        agent_checkpoint();
    }
}
