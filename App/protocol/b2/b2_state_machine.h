#ifndef B2_STATE_MACHINE_H
#define B2_STATE_MACHINE_H

#include "b2_protocol.h"

typedef enum {
    B2_RX_HEADER,
    B2_RX_DATA,
    B2_RX_COMPLETE,
    B2_RX_DROP
} b2_rx_state_t;

typedef struct {
    b2_rx_state_t state;        //接收状态机状态
    size_t used;                //已接收字节数
    size_t expected;            //期望接收字节数
    uint8_t frame[B2_MAX_FRAME];//接收缓冲区，包含头、数据、尾
} b2_state_machine_t;

/** 每次CS拉低开始一个全新事务，禁止把两次CS拼成一帧。 */
void b2_sm_begin(b2_state_machine_t *sm);
/** 按收到顺序喂入字节。首字节FF是主机读占位，整次CS忽略；坏帧不扫描正文找5A。 */
void b2_sm_feed(b2_state_machine_t *sm, uint8_t byte);
/** 只在CS拉高后调用；完整帧返回1，其余返回0。解析结果由request.status进一步区分。 */
int b2_sm_end(const b2_state_machine_t *sm, b2_request_t *request);

#endif
