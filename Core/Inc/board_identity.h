#ifndef BOARD_IDENTITY_H
#define BOARD_IDENTITY_H

#include <stdint.h>

typedef enum {
    BOARD_PHY_YT8512H = 0,
} board_phy_model_t;

void board_mac_from_uid(const uint32_t uid[3], uint8_t mac[6]);
const uint8_t *board_mac_address(void);
board_phy_model_t board_phy_model(void);

/**
 * 读取三位硬件槽位码，返回uint8_t的0～7，不加1，不读取或写入FRU。
 * 必须在PB3/PB4/PB5已配置为GPIO输入、无内部上下拉后调用。
 * 位序为PB5:PB4:PB3，PB3为最低位；外部上拉为1，B2贴0Ω接地为0。
 * 111也是有效码7；本接口不能判断是否插在B2上，不用于自行分配IP。
 */
uint8_t board_slot_code(void);

#endif
