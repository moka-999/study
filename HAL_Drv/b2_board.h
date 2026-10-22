#ifndef B2_BOARD_H
#define B2_BOARD_H

#include <stdint.h>

/** B1每次读取PB3..5+1；V2使用编译配置B2_TEST_SITE_ID（默认1）。不写FRU。 */
uint8_t b2_board_site_id(void);

#endif
