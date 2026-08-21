#ifndef BOARD_IDENTITY_H
#define BOARD_IDENTITY_H

#include <stdint.h>

void board_mac_from_uid(const uint32_t uid[3], uint8_t mac[6]);
const uint8_t *board_mac_address(void);

#endif
