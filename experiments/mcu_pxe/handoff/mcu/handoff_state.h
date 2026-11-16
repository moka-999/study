#ifndef HANDOFF_STATE_H
#define HANDOFF_STATE_H
#include <stdint.h>

/* 仅描述地址所有权；EDK_IDLE仍是串口V2，确定掉电后才回正式TLV。 */
typedef enum {
    HANDOFF_NORMAL = 0, HANDOFF_ARMED, HANDOFF_WITHDRAWING,
    HANDOFF_OWNED, HANDOFF_RELEASING, HANDOFF_RESTORED, HANDOFF_EDK_IDLE
} handoff_phase_t;
typedef struct {
    handoff_phase_t phase;
    uint32_t session;
    uint8_t authorized, seen_on, off_release;
} handoff_state_t;

/* off/idle均为布尔值；仅OFF且业务空闲可arm，失败不改变状态。 */
int handoff_state_arm(handoff_state_t *s, int off, int idle);
/* 返回SN状态；首次HELLO只记录session，撤IP完成后才允许授权。 */
uint8_t handoff_state_hello(handoff_state_t *s, uint32_t session);
uint8_t handoff_state_release(handoff_state_t *s, uint32_t session);
/* on/off分别表示确实ON/OFF，过渡态两者均0；arm时原有OFF不触发归还。
 * EDK_IDLE的OFF调用前须由tcpip确认没有待发包/DMA所有权。 */
int handoff_state_power(handoff_state_t *s, int on, int off);
int handoff_state_cancel(handoff_state_t *s, int off);
int handoff_state_can_withdraw(const handoff_state_t *s, int connected,
                               int on, int idle, int uart_idle);
int handoff_state_listen(const handoff_state_t *s);
/* 必须在IP恢复且listen已建立后调用；OFF归还清会话，其余保持EDK_IDLE。 */
void handoff_state_listener_ready(handoff_state_t *s);
#endif
