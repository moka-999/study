#include "board_identity.h"

#include "main.h"

void board_mac_from_uid(const uint32_t uid[3], uint8_t mac[6])
{
    uint64_t hash = UINT64_C(1469598103934665603);

    if (!uid || !mac) return;
    for (uint8_t word = 0; word < 3; word++) {
        for (uint8_t byte = 0; byte < 4; byte++) {
            hash ^= (uint8_t)(uid[word] >> (byte * 8));
            hash *= UINT64_C(1099511628211);
        }
    }

    mac[0] = 0x02; /* 本地管理的单播MAC，不使用广播或组播地址。 */
    for (uint8_t i = 1; i < 6; i++)
        mac[i] = (uint8_t)(hash >> ((i - 1) * 8));
}

const uint8_t *board_mac_address(void)
{
    static uint8_t mac[6];
    if (mac[0] == 0) {
        const uint32_t uid[3] = { HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2() };
        board_mac_from_uid(uid, mac);
    }
    return mac;
}

board_phy_model_t board_phy_model(void)
{
    return BOARD_PHY_YT8512H;
}

uint8_t board_slot_code(void)
{
    /* 三脚同属GPIOB，一次取样，避免三次读取拼出跨时刻电平。 */
    const uint32_t levels = B1_SLOT_ID0_PORT->IDR;
    return (uint8_t)(((levels & B1_SLOT_ID0_PIN) != 0U ? 1U : 0U) |
                     ((levels & B1_SLOT_ID1_PIN) != 0U ? 2U : 0U) |
                     ((levels & B1_SLOT_ID2_PIN) != 0U ? 4U : 0U));
}
