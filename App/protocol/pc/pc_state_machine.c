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

/*
 * HEADER：只收固定头。Length/Version/Flag/ServiceID分别在第4/6/8/10字节
 * 到齐时校验，
 * 不必等到18字节甚至整个正文收齐；因此非法大包仍能尽早被拒绝。
 * 收满头后由Length决定进入BODY还是直接FRAME_READY（无正文的18字节帧）。
 * 正文是否是合法JSON由上层决定，帧状态机不替JSON层扩大或缩小接收规则。
 */
static pc_sm_feed_result_t feed_header(pc_sm_t *sm, uint8_t byte)
{
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
    if (sm->received == 10U && read_be16(&sm->buffer[8]) != 0U) {
        return fail(sm, PC_SM_ERROR_SERVICE_ID);
    }

    if (sm->received == PC_PROTOCOL_HEADER_SIZE) {
        if (sm->expected_length == PC_PROTOCOL_HEADER_SIZE) {
            sm->state = PC_SM_STATE_FRAME_READY;
            return PC_SM_FEED_FRAME_READY;
        }
        sm->state = PC_SM_STATE_BODY;
    }
    return PC_SM_FEED_MORE;
}

/* BODY：头部已经校验通过，只收正文并在完整PDU到齐时通知调用方。 */
static pc_sm_feed_result_t feed_body(pc_sm_t *sm, uint8_t byte)
{
    sm->buffer[sm->received++] = byte;
    if (sm->received == sm->expected_length) {
        sm->state = PC_SM_STATE_FRAME_READY;
        return PC_SM_FEED_FRAME_READY;
    }
    return PC_SM_FEED_MORE;
}

/*
 * 两个停止接收的状态共用同一处理函数：FRAME_READY要先取帧再reset，
 * ERROR由TCP任务关闭连接。继续feed属于调用错误，不写buffer、不递增
 * received，避免覆盖上一帧或把坏连接中的后续字节误当成新帧。
 */
static pc_sm_feed_result_t reject_byte(pc_sm_t *sm, uint8_t byte)
{
    (void)byte;
    return fail(sm, PC_SM_ERROR_STATE);
}

typedef pc_sm_feed_result_t (*pc_sm_state_handler_t)(pc_sm_t *sm, uint8_t byte);

/*
 * 状态查表入口：枚举值直接作为下标，四个状态的字节处理职责在此集中列出。
 * 增加状态时，在头文件COUNT之前声明枚举、编写处理函数并登记本表即可；
 * 不需要在TCP任务里追加状态分支。本表只路由解析状态，不路由业务命令，
 * CommandID -> handler仍由pc_dispatch中的独立命令表负责。
 */
static const pc_sm_state_handler_t s_state_handlers[] = {
    [PC_SM_STATE_HEADER]      = feed_header,
    [PC_SM_STATE_BODY]        = feed_body,
    [PC_SM_STATE_FRAME_READY] = reject_byte,
    [PC_SM_STATE_ERROR]       = reject_byte,
};

_Static_assert(sizeof(s_state_handlers) / sizeof(s_state_handlers[0]) ==
               PC_SM_STATE_COUNT, "PC state table must cover the state enum");

pc_sm_feed_result_t pc_sm_feed(pc_sm_t *sm, uint8_t byte)
{
    if (!sm) return PC_SM_FEED_ERROR;

    /* 必须先检查下标再取函数指针；转成无符号也可拦截负值枚举。
     * 缓冲上限统一在写入前检查，所有可接收状态都受同一4KiB限制。 */
    if ((unsigned int)sm->state >= PC_SM_STATE_COUNT ||
        sm->received >= PC_PROTOCOL_MAX_PDU) {
        return fail(sm, PC_SM_ERROR_STATE);
    }
    pc_sm_state_handler_t handler = s_state_handlers[sm->state];
    if (!handler) return fail(sm, PC_SM_ERROR_STATE);
    return handler(sm, byte);
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
