#include "handoff_state.h"
#include "serial_net.h"
#include <string.h>

int handoff_state_arm(handoff_state_t *s, int off, int idle)
{
    if (s->phase != HANDOFF_NORMAL || !off || !idle) return 0;
    memset(s, 0, sizeof(*s));
    s->phase = HANDOFF_ARMED;
    return 1;
}

uint8_t handoff_state_hello(handoff_state_t *s, uint32_t session)
{
    if (!session || s->phase == HANDOFF_NORMAL ||
        (s->session && s->session != session)) return SN_INVALID;
    if (s->phase == HANDOFF_ARMED && !s->session) s->session = session;
    if (s->phase != HANDOFF_OWNED) return SN_NOT_READY;
    s->authorized = 1U;
    return SN_OK;
}

uint8_t handoff_state_release(handoff_state_t *s, uint32_t session)
{
    if (!s->authorized || session != s->session) return SN_INVALID;
    if (s->phase == HANDOFF_EDK_IDLE) return SN_OK;
    if (s->phase == HANDOFF_OWNED) s->phase = HANDOFF_RELEASING;
    return SN_NOT_READY;
}

int handoff_state_power(handoff_state_t *s, int on, int off)
{
    if (s->phase == HANDOFF_NORMAL) return 0;
    if (on) s->seen_on = 1U;
    if (!off || !s->seen_on || s->off_release) return 0;
    /* 地址/listen已归还后，真实掉电只清串口会话，不再断开新PC连接。 */
    if (s->phase == HANDOFF_EDK_IDLE) {
        memset(s, 0, sizeof(*s));
        return 1;
    }
    s->off_release = 1U;
    s->phase = HANDOFF_RELEASING;
    return 1;
}

int handoff_state_cancel(handoff_state_t *s, int off)
{
    if (!off || s->phase == HANDOFF_NORMAL) return 0;
    s->off_release = 1U;
    s->phase = HANDOFF_RELEASING;
    return 1;
}

int handoff_state_can_withdraw(const handoff_state_t *s, int connected,
                               int on, int idle, int uart_idle)
{
    return s->phase == HANDOFF_ARMED && s->session && !connected &&
           on && idle && uart_idle;
}

int handoff_state_listen(const handoff_state_t *s)
{
    return s->phase == HANDOFF_NORMAL || s->phase == HANDOFF_ARMED ||
           s->phase == HANDOFF_RESTORED || s->phase == HANDOFF_EDK_IDLE;
}

void handoff_state_listener_ready(handoff_state_t *s)
{
    if (s->phase != HANDOFF_RESTORED) return;
    if (s->off_release) memset(s, 0, sizeof(*s));
    else s->phase = HANDOFF_EDK_IDLE;
}
