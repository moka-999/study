#include "pc_state_machine.h"

/*
 * 固定头使用网络大端序。逐字节移位既适配STM32H743小端CPU，也避免
 * 将未对齐的网络缓冲区强转成uint16_t/uint32_t后解引用。
 */
static uint16_t read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static pc_sm_feed_result_t fail(pc_sm_t *sm, pc_sm_error_t error)
{
    sm->state = PC_SM_STATE_ERROR;
    sm->error = error;
    return PC_SM_FEED_ERROR;
}

void pc_sm_reset(pc_sm_t *sm)
{
    if (!sm) return;
    sm->state = PC_SM_STATE_HEADER;
    sm->received = 0U;
    sm->expected_length = 0U;
    sm->error = PC_SM_ERROR_NONE;
}

void pc_sm_init(pc_sm_t *sm)
{
    pc_sm_reset(sm);
}

pc_sm_feed_result_t pc_sm_feed(pc_sm_t *sm, uint8_t byte)
{
    /* FRAME_READY必须先取帧并reset；ERROR连接也不允许尝试原地重同步。 */
    if (!sm || sm->state == PC_SM_STATE_FRAME_READY ||
        sm->state == PC_SM_STATE_ERROR || sm->received >= PC_PROTOCOL_MAX_PDU) {
        if (sm) return fail(sm, PC_SM_ERROR_STATE);
        return PC_SM_FEED_ERROR;
    }

    sm->buffer[sm->received++] = byte;

    /* Length刚到齐便拒绝异常值，避免继续接收不可信的大包体。 */
    if (sm->received == 4U) {
        sm->expected_length = read_be32(sm->buffer);
        if (sm->expected_length < PC_PROTOCOL_HEADER_SIZE ||
            sm->expected_length > PC_PROTOCOL_MAX_PDU) {
            return fail(sm, PC_SM_ERROR_LENGTH);
        }
    }

    /* Version和Flag位于头部前段，不必等JSON正文到齐后才发现错误。 */
    if (sm->received == 6U &&
        read_be16(&sm->buffer[4]) != PC_PROTOCOL_VERSION) {
        return fail(sm, PC_SM_ERROR_VERSION);
    }
    if (sm->received == 8U &&
        read_be16(&sm->buffer[6]) != PC_PROTOCOL_FLAG_JSON) {
        return fail(sm, PC_SM_ERROR_FLAG);
    }

    if (sm->expected_length != 0U &&
        sm->received == sm->expected_length) {
        sm->state = PC_SM_STATE_FRAME_READY;
        return PC_SM_FEED_FRAME_READY;
    }
    if (sm->received >= PC_PROTOCOL_HEADER_SIZE) {
        sm->state = PC_SM_STATE_BODY;
    }
    return PC_SM_FEED_MORE;
}

int pc_sm_get_frame(const pc_sm_t *sm, pc_protocol_frame_t *frame)
{
    if (!sm || !frame || sm->state != PC_SM_STATE_FRAME_READY ||
        sm->error != PC_SM_ERROR_NONE ||
        sm->expected_length != sm->received) {
        return -1;
    }

    /*
     * 固定18字节头布局：
     *  0..3  Length      4..5  Version      6..7  Flag
     *  8..9  ServiceID  10..11 CommandID   12..15 Seq
     * 16..17 Reserved   18..   UTF-8 JSON正文
     */
    frame->length = read_be32(&sm->buffer[0]);
    frame->version = read_be16(&sm->buffer[4]);
    frame->flag = read_be16(&sm->buffer[6]);
    frame->service_id = read_be16(&sm->buffer[8]);
    frame->command_id = read_be16(&sm->buffer[10]);
    frame->sequence = read_be32(&sm->buffer[12]);
    frame->reserved = read_be16(&sm->buffer[16]);
    frame->json = &sm->buffer[PC_PROTOCOL_HEADER_SIZE];
    frame->json_length = frame->length - PC_PROTOCOL_HEADER_SIZE;
    return 0;
}

const uint8_t *pc_sm_frame_data(const pc_sm_t *sm)
{
    return sm && sm->state == PC_SM_STATE_FRAME_READY ? sm->buffer : NULL;
}

int pc_sm_is_receiving(const pc_sm_t *sm)
{
    return sm && sm->received != 0U &&
           (sm->state == PC_SM_STATE_HEADER || sm->state == PC_SM_STATE_BODY);
}

pc_sm_error_t pc_sm_error(const pc_sm_t *sm)
{
    return sm ? sm->error : PC_SM_ERROR_STATE;
}
