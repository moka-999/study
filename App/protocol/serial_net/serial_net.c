#include "serial_net.h"

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t value)
{
    unsigned int i;
    for (i = 0; i < 4U; ++i) p[i] = (uint8_t)(value >> (8U * i));
}

uint32_t sn_crc32(const uint8_t *data, uint16_t length)
{
    uint32_t crc = 0xffffffffU;
    unsigned int i, bit;
    for (i = 0; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8U; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return ~crc;
}

uint16_t sn_pack(const sn_frame_t *f, uint8_t *wire, uint16_t capacity)
{
    uint8_t raw[SN_RAW_MAX];
    uint16_t n, i, out = 1, mark = 0;
    uint8_t code = 1;
    if (!f || !wire || !f->session || f->length > SN_ETH_MAX || capacity < SN_WIRE_MAX)
        return 0;
    raw[0] = SN_VERSION; raw[1] = f->op; raw[2] = f->status; raw[3] = 0;
    put32(raw + 4, f->session); put32(raw + 8, f->sequence);
    raw[12] = (uint8_t)f->length; raw[13] = (uint8_t)(f->length >> 8);
    for (i = 0; i < f->length; ++i) raw[14U + i] = f->data[i];
    n = (uint16_t)(14U + f->length);
    put32(raw + n, sn_crc32(raw, n)); n += 4U;
    for (i = 0; i < n; ++i) {
        if (raw[i] == 0U) {
            wire[mark] = code; mark = out++; code = 1;
        } else {
            wire[out++] = raw[i];
            if (++code == 0xffU) { wire[mark] = code; mark = out++; code = 1; }
        }
    }
    wire[mark] = code; wire[out++] = 0;
    return out;
}

void sn_parser_reset(sn_parser_t *p)
{
    p->used = 0; p->overflow = 0;
}

int sn_feed(sn_parser_t *p, uint8_t byte, sn_frame_t *f)
{
    uint8_t raw[SN_RAW_MAX];
    uint16_t i = 0, n = 0, used;
    if (byte != 0U) {
        if (p->used < sizeof(p->encoded)) p->encoded[p->used++] = byte;
        else p->overflow = 1;
        return 0;
    }
    used = p->used; p->used = 0;
    if (p->overflow) { p->overflow = 0; return -1; }
    if (!used) return 0;
    while (i < used) {
        uint8_t code = p->encoded[i++], j;
        if (!code || (unsigned int)i + code - 1U > used) return -1;
        for (j = 1; j < code; ++j) {
            if (n >= sizeof(raw)) return -1;
            raw[n++] = p->encoded[i++];
        }
        if (code != 0xffU && i < used) {
            if (n >= sizeof(raw)) return -1;
            raw[n++] = 0;
        }
    }
    if (n < 18U || raw[0] != SN_VERSION || raw[3] != 0U || !get32(raw + 4)) return -1;
    used = (uint16_t)((uint16_t)raw[12] | (uint16_t)raw[13] << 8);
    if (used > SN_ETH_MAX || n != used + 18U || sn_crc32(raw, n - 4U) != get32(raw + n - 4U))
        return -1;
    f->op = raw[1]; f->status = raw[2]; f->session = get32(raw + 4);
    f->sequence = get32(raw + 8); f->length = used;
    for (i = 0; i < used; ++i) f->data[i] = raw[14U + i];
    return 1;
}

static int same_request(const sn_frame_t *a, const sn_frame_t *b)
{
    unsigned int i;
    if (a->op != b->op || a->status != b->status || a->length != b->length) return 0;
    for (i = 0; i < a->length; ++i) if (a->data[i] != b->data[i]) return 0;
    return 1;
}

int sn_server_handle(sn_server_t *s, const sn_frame_t *q, sn_frame_t *r,
                     sn_execute_t execute, void *context)
{
    r->op = q->op | SN_REPLY; r->session = q->session; r->sequence = q->sequence;
    r->status = SN_INVALID; r->length = 0;
    if (!q->session || q->status || q->length > SN_ETH_MAX ||
        q->op < SN_HELLO || q->op > SN_RELEASE) return -1;
    if (s->valid && s->request.session == q->session) {
        if (s->request.sequence == q->sequence) {
            if (!same_request(&s->request, q)) return -1;
            *r = s->response; return 0;
        }
        if (q->sequence != s->request.sequence + 1U) {
            /* 旧SEND可能根本未到达，显式归还不能为补序号而重发未知数据。
             * 仅同session、空RELEASE允许前向跳号；半圈之外及旧序号仍拒绝。
             */
            uint32_t forward = q->sequence - s->request.sequence;
            const uint32_t forward_limit = 0x7fffffffU;
            if (q->op != SN_RELEASE || q->length != 0U ||
                forward == 0U || forward > forward_limit) return -1;
        }
    } else if (s->valid || q->op != SN_HELLO || q->length != 0U) {
        return -1;
    }
    execute(context, q, r);
    if (r->length > SN_ETH_MAX) { r->length = 0; r->status = SN_IO_ERROR; }
    s->request = *q; s->response = *r; s->valid = 1;
    return 1;
}
