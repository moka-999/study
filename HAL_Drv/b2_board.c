#include "b2_board.h"
#if defined(B1_RK_UART)
#include "board_identity.h"
#else
#ifndef B2_TEST_SITE_ID
#define B2_TEST_SITE_ID 1
#endif
_Static_assert(B2_TEST_SITE_ID >= 1 && B2_TEST_SITE_ID <= 8, "V2测试槽位必须为1..8");
#endif

uint8_t b2_board_site_id(void)
{
#if defined(B1_RK_UART)
    return (uint8_t)(board_slot_code() + 1U);
#else
    return B2_TEST_SITE_ID;
#endif
}
