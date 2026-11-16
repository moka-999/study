/* 默认空闲供普通FUNC/PARA/B2回归使用；PC自动SLT用例显式控制交接生命周期。 */
#include "handoff_test.h"

handoff_test_state_t handoff_test = {.cancel_allowed = 1};

void handoff_test_reset(void)
{
    handoff_test = (handoff_test_state_t){.cancel_allowed = 1};
}

void handoff_test_set_status(handoff_slt_status_t status)
{
    handoff_test.status = status;
    if (status == HANDOFF_SLT_OFF_READY) handoff_test.reserved = 0;
}

int handoff_slt_begin(uint32_t cookie)
{
    ++handoff_test.begin_calls;
    if (!cookie || handoff_test.reserved || handoff_test.begin_failure) return 0;
    handoff_test.cookie = cookie;
    handoff_test.reserved = 1;
    handoff_test.prepared = handoff_test.cancelled = 0;
    handoff_test.status = HANDOFF_SLT_WAITING;
    return 1;
}

int handoff_slt_prepared(uint32_t cookie)
{
    ++handoff_test.prepared_calls;
    if (cookie != handoff_test.cookie || !handoff_test.reserved ||
        handoff_test.cancelled || handoff_test.prepared_failure) return 0;
    handoff_test.prepared = 1;
    return 1;
}

handoff_slt_status_t handoff_slt_status(uint32_t cookie)
{
    return cookie && cookie == handoff_test.cookie ? handoff_test.status : HANDOFF_SLT_FAILED;
}

int handoff_slt_cancel(uint32_t cookie)
{
    ++handoff_test.cancel_calls;
    if (!cookie || cookie != handoff_test.cookie || !handoff_test.cancel_allowed) return 0;
    handoff_test.cancelled = 1;
    /* 取消预约后仍等待RK任务复位串口；OFF_READY由用例单独确认。 */
    return 1;
}

int handoff_mcu_busy(void) { return handoff_test.reserved; }
void handoff_mcu_stop(void) { handoff_test.reserved = 1; }
int handoff_mcu_power_allowed(int off_command) { return off_command || !handoff_test.reserved; }
int handoff_mcu_rk_poll(void) { return 0; }
int handoff_mcu_rk_observe(uint8_t byte) { (void)byte; return 0; }
void handoff_mcu_rk_reset_probe(void) { }
void handoff_mcu_rk_wait(void) { }
int handoff_mcu_console_request(handoff_command_t command) { (void)command; return 0; }
