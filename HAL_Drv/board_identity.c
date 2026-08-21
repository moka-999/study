#include "board_identity.h"

#if BOARD_PROFILE == BOARD_PROFILE_B1
#include "stm32h7xx_hal.h"
#endif

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

    mac[0] = 0x02; /* locally administered, unicast */
    for (uint8_t i = 1; i < 6; i++)
        mac[i] = (uint8_t)(hash >> ((i - 1) * 8));
}

const uint8_t *board_mac_address(void)
{
#if BOARD_PROFILE == BOARD_PROFILE_B1
    static uint8_t mac[6];
    if (mac[0] == 0) {
        const uint32_t uid[3] = { HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2() };
        board_mac_from_uid(uid, mac);
    }
    return mac;
#else
    static const uint8_t mac[6] = { 0x00, 0x80, 0xE1, 0x00, 0x00, 0x00 };
    return mac;
#endif
}
