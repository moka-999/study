#ifndef BOARD_IDENTITY_H
#define BOARD_IDENTITY_H

#include <stdint.h>

#define BOARD_PROFILE_APOLLO_V2  0
#define BOARD_PROFILE_B1         1

#ifndef BOARD_PROFILE
#define BOARD_PROFILE BOARD_PROFILE_APOLLO_V2
#endif

typedef enum {
    BOARD_PHY_YT8512C = 0,
    BOARD_PHY_YT8512H = 1,
} board_phy_model_t;

void board_mac_from_uid(const uint32_t uid[3], uint8_t mac[6]);
const uint8_t *board_mac_address(void);
board_phy_model_t board_phy_model(void);

#endif
