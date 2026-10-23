#include "state_machine.h"
#include "crc.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

void tlv_sm_init(tlv_sm_t *sm)
{
    if (sm != NULL) memset(sm, 0, sizeof(*sm));
}
void tlv_sm_reset(tlv_sm_t *sm)
{
    if (sm == NULL) return;
    sm->state = TLV_SM_IDLE;
    sm->pos = 0U;
    sm->expect = 0U;
    sm->ready_len = 0U;
}
int tlv_sm_check_timeout(tlv_sm_t *sm, uint32_t timeout_ms)
{
    if (sm == NULL || sm->state == TLV_SM_IDLE) return 0;
    if ((uint32_t)(xTaskGetTickCount() - sm->last_tick) >= pdMS_TO_TICKS(timeout_ms)) {
        tlv_sm_reset(sm);
        return 1;
    }
    return 0;
}
/* 头校验失败时保留后续可能的5A前缀；头完整长度始终是5，不是剩余4字节。 */
static void resync_header(tlv_sm_t *sm)
{
    // 如果头校验失败，尝试在缓冲区保留后续5A前缀，重新收集帧头。
    for (uint16_t i = 1U; i < sm->pos; ++i) {
        if (sm->buf[i] == TLV_SYNC_HEADER) {
            sm->pos -= i;
            memmove(sm->buf, sm->buf + i, sm->pos);
            sm->state = TLV_SM_HEADER;
            sm->expect = 5U;
            return;
        }
    }
    //未找到5A，重置状态机
    tlv_sm_reset(sm);
}
/* 状态处理函数共用返回约定：0继续收集、1完整帧边界成立、-1错误已恢复。
 * 调用前feed已经把byte写入buf，pos指向已收字节数；handler只推进本阶段。
 */
typedef int (*rk_state_handler_t)(tlv_sm_t *sm, uint8_t byte);

/* 没有在途帧：只接受5A，其余字节视为噪声。 */
static int handle_idle(tlv_sm_t *sm, uint8_t byte)
{
    if (byte == TLV_SYNC_HEADER) {
        sm->state = TLV_SM_HEADER;
        sm->expect = 5U; /* 帧头1 + Length2 + HCRC2。 */
    } else {
        sm->pos = 0U;
    }
    return 0;
}

/* 头部可信后才使用Length接收数据；坏头在已有5字节内尝试寻找下一个5A。 */
static int handle_header(tlv_sm_t *sm, uint8_t byte)
{
    (void)byte;
    if (sm->pos < sm->expect) return 0;
    //读取length字段
    uint16_t length = tlv_get_length(sm->buf + 1);
    //检查hcrc字段
    if (crc16_modbus(sm->buf, 5U) != 0U ||
        (length != 0U && length < TLV_PAYLOAD_MIN) || length > TLV_PAYLOAD_MAX) {
        resync_header(sm);
        return 0;
    }
    //头部校验通过，切换到下个状态和接收期望
    sm->state = TLV_SM_DATA;
    sm->expect = 13U + length; /* 头5 + Seq8 + TLV区Length。 */
    return 0;
}

/* DATA包含Seq和TLV区，必须收满后才切到FCRC阶段。 */
static int handle_data(tlv_sm_t *sm, uint8_t byte)
{
    //按长度收集Seq+TLV区，直到达到期望长度
    (void)byte;
    if (sm->pos == sm->expect) {
        sm->state = TLV_SM_CRC;
        sm->expect += 2U;
    }
    return 0;
}

/* 接收到的FCRC也参与计算；对Seq+TLV+FCRC重算余数0即通过。 */
static int handle_crc(tlv_sm_t *sm, uint8_t byte)
{
    //尾部crc校验
    (void)byte;
    if (sm->pos < sm->expect) return 0;
    if (crc16_modbus(sm->buf + 5, sm->pos - 5U) != 0U) {
        tlv_sm_reset(sm);
        return -1;
    }
    sm->state = TLV_SM_FOOTER;
    ++sm->expect;
    return 0;
}

/* 发布长度但不提前交付业务；get_frame仍要检查TLV内部结构。 */
static int handle_footer(tlv_sm_t *sm, uint8_t byte)
{
    //检查帧尾是否为A5，否则重置状态机
    if (byte == TLV_SYNC_FOOTER) {
        uint16_t length = sm->pos;
        tlv_sm_reset(sm);
        sm->ready_len = length;
        return 1;
    }
    // 帧尾错误时重置状态机；若当前字节恰为5A，就将它保留为新帧头。
    tlv_sm_reset(sm);
    /* 缺失帧尾后紧接新帧头时，不吞掉下一帧的5A。 */
    if (byte == TLV_SYNC_HEADER) {
        sm->buf[0] = byte;
        sm->pos = 1U;
        sm->state = TLV_SM_HEADER;
        sm->expect = 5U;
    }
    return -1;
}

/* 接收状态分发表：每个状态对应一个处理函数；新增状态必须同时补表和测试。
 * 这是字节流状态分发，不是RK算法分发，也不会执行电源/PC业务。
 */
static const rk_state_handler_t s_state_handlers[] = {
    [TLV_SM_IDLE]   = handle_idle,
    [TLV_SM_HEADER] = handle_header,
    [TLV_SM_DATA]   = handle_data,
    [TLV_SM_CRC]    = handle_crc,
    [TLV_SM_FOOTER] = handle_footer,
};

_Static_assert(sizeof(s_state_handlers) / sizeof(s_state_handlers[0]) == TLV_SM_COUNT,
               "RK状态分发表必须覆盖全部接收状态");

int tlv_sm_feed(tlv_sm_t *sm, uint8_t byte)
{
    if (sm == NULL) return -1;

    // 如果状态值不在有效范围内，或者对应的处理函数为空，则重置状态机并返回错误。
    if ((unsigned)sm->state >= TLV_SM_COUNT || s_state_handlers[sm->state] == NULL) {
        tlv_sm_reset(sm);
        return -1;
    }
    //检查半帧是否超过100ms未收到新字节，如果超过则重置状态机
    (void)tlv_sm_check_timeout(sm, RK_BYTE_TIMEOUT_MS);
    // 将当前字节写入缓冲区，并更新位置和时间戳
    sm->ready_len = 0U;
    sm->last_tick = xTaskGetTickCount();
    if (sm->pos >= TLV_FRAME_MAX) tlv_sm_reset(sm);
    sm->buf[sm->pos++] = byte;
    // 调用当前状态对应的处理函数，传入状态机和当前字节，返回处理结果
    return s_state_handlers[sm->state](sm, byte);
}
int tlv_sm_get_frame(tlv_sm_t *sm, tlv_frame_t *frame)
{
    if (sm == NULL || sm->ready_len == 0U) {
        frame_init(frame);
        return -1;
    }
    //将完整的缓冲帧交给tlv_unpack函数进行解析，并将结果存储在frame中，返回解析结果
    return tlv_unpack(sm->buf, sm->ready_len, frame);
}
