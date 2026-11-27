#ifndef HANDOFF_TEST_H
#define HANDOFF_TEST_H

#include "handoff_mcu.h"

/* PC业务回归显式推进交接状态；不模拟UART、EDK执行或真实硬件确认。 */
typedef struct {
    uint32_t cookie;
    handoff_slt_status_t status;
    unsigned begin_calls, prepared_calls, cancel_calls;
    int reserved, prepared, cancelled;
    int uart_handoff; /* 1 表示串口已离开 SPL；仅预约时仍可发送时序查询。 */
    int begin_failure, prepared_failure, cancel_allowed;
} handoff_test_state_t;

extern handoff_test_state_t handoff_test;
/* 仅在用例边界恢复空闲；单个生命周期必须显式给出RELEASED和OFF_READY。 */
void handoff_test_reset(void);
void handoff_test_set_status(handoff_slt_status_t status);

#endif
