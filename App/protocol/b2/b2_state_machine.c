#include "b2_state_machine.h"
#include "crc.h"

void b2_sm_begin(b2_state_machine_t *sm)
{
    if (!sm) return;
    sm->state = B2_RX_HEADER;
    sm->used = 0U;
    sm->expected = 0U;
}

static void header_byte(b2_state_machine_t *sm, uint8_t byte)
{
    if (sm->used == 0U && byte != 0x5AU) { sm->state = B2_RX_DROP; return; }
    sm->frame[sm->used++] = byte;
    if (sm->used == 5U) {
        size_t payload = ((size_t)sm->frame[1] << 8) | sm->frame[2];
        uint16_t crc = (uint16_t)(sm->frame[3] | ((uint16_t)sm->frame[4] << 8));
        if (payload < 6U || payload > B2_MAX_PAYLOAD ||
            crc != crc16_modbus(sm->frame, 3U)) { sm->state = B2_RX_DROP; return; }
        sm->expected = payload + 16U;
    }
    if (sm->used == B2_HEADER_SIZE) sm->state = B2_RX_DATA;
}

static void data_byte(b2_state_machine_t *sm, uint8_t byte)
{
    sm->frame[sm->used++] = byte;
    if (sm->used == sm->expected) sm->state = B2_RX_COMPLETE;
}

static void extra_byte(b2_state_machine_t *sm, uint8_t byte)
{
    (void)byte;
    /* 尾部后多一字节也不是合法请求，不执行已经看到的前缀帧。 */
    sm->state = B2_RX_DROP;
}

static void ignored_byte(b2_state_machine_t *sm, uint8_t byte)
{
    (void)sm; (void)byte;
}

void b2_sm_feed(b2_state_machine_t *sm, uint8_t byte)
{
    static void (*const handlers[])(b2_state_machine_t *, uint8_t) = {
        header_byte, data_byte, extra_byte, ignored_byte
    };
    if (!sm || (unsigned int)sm->state >= sizeof(handlers) / sizeof(handlers[0])) return;
    handlers[sm->state](sm, byte);
}

int b2_sm_end(const b2_state_machine_t *sm, b2_request_t *request)
{
    return sm && sm->state == B2_RX_COMPLETE &&
           b2_request_decode(sm->frame, sm->used, request);
}
