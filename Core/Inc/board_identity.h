#ifndef BOARD_IDENTITY_H
#define BOARD_IDENTITY_H

#include <stdint.h>

typedef enum {
    BOARD_PHY_YT8512H = 0,
} board_phy_model_t;

void board_mac_from_uid(const uint32_t uid[3], uint8_t mac[6]);
const uint8_t *board_mac_address(void);
board_phy_model_t board_phy_model(void);

#endif
