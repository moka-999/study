#include "serial_net.h"
#include <string.h>
#include <stdint.h>
#define S0(x) #x
#define S(x) S0(x)
#define CHECK(x) do { if (!(x)) fail("FAIL " S(__LINE__) " " #x "\n"); } while (0)
static void message(const char *s)
{
    register uint32_t r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = s;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}
static _Noreturn void finish(uint32_t code)
{
    uint32_t block[] = {0x20026U, code};
    register uint32_t r0 __asm__("r0") = 0x20U;
    register uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) {}
}
static _Noreturn void fail(const char *s) { message(s); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }
static sn_frame_t q, r, decoded;
static sn_parser_t parser;
static sn_server_t server;
static uint8_t wire[SN_WIRE_MAX];
static unsigned executions;
static void execute(void *unused, const sn_frame_t *a, sn_frame_t *b)
{
    (void)unused;
    ++executions; b->status = SN_OK; b->length = 1; b->data[0] = a->op;
}
void test_main(void)
{
    CHECK(sn_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926U);
    q.session = 0xa5000099U; q.sequence = 0xffffffffU; q.op = SN_SEND;
    for (unsigned size = 0; size <= SN_ETH_MAX; ++size) {
        q.length = size;
        for (unsigned i = 0; i < size; ++i) q.data[i] = (uint8_t)(i * 11U);
        unsigned n = sn_pack(&q, wire, sizeof(wire));
        CHECK(n && n <= sizeof(wire));
        for (unsigned i = 0; i < n; ++i) CHECK(sn_feed(&parser, wire[i], &decoded) == (i == n - 1));
        CHECK(decoded.session == q.session && decoded.sequence == q.sequence && decoded.length == size);
        CHECK(!memcmp(decoded.data, q.data, size));
    }
    q.length = 100; unsigned n = sn_pack(&q, wire, sizeof(wire));
    wire[n - 3] ^= 0x40;
    int result = 0;
    for (unsigned i = 0; i < n; ++i) result = sn_feed(&parser, wire[i], &decoded);
    CHECK(result < 0);
    for (unsigned i = 0; i < SN_WIRE_MAX + 10; ++i) CHECK(sn_feed(&parser, 1, &decoded) == 0);
    CHECK(sn_feed(&parser, 0, &decoded) < 0);
    n = sn_pack(&q, wire, sizeof(wire));
    for (unsigned i = 0; i < n; ++i) result = sn_feed(&parser, wire[i], &decoded);
    CHECK(result == 1);
    q.op = SN_HELLO; q.length = 0;
    CHECK(sn_server_handle(&server, &q, &r, execute, 0) == 1 && executions == 1);
    CHECK(sn_server_handle(&server, &q, &r, execute, 0) == 0 && executions == 1);
    q.op = SN_SEND;
    CHECK(sn_server_handle(&server, &q, &r, execute, 0) == -1);
    q.sequence = 0;
    CHECK(sn_server_handle(&server, &q, &r, execute, 0) == 1 && executions == 2);
    CHECK(sn_server_handle(&server, &q, &r, execute, 0) == 0 && executions == 2);
    q.sequence = 2;
    CHECK(sn_server_handle(&server, &q, &r, execute, 0) == -1);
    q.sequence = 1; q.op = SN_POLL;
    CHECK(sn_server_handle(&server, &q, &r, execute, 0) == 1 && executions == 3);
    CHECK(sn_server_handle(&server, &q, &r, execute, 0) == 0 && executions == 3);
    q.session++;
    CHECK(sn_server_handle(&server, &q, &r, execute, 0) == -1);
    q.op = SN_HELLO;
    CHECK(sn_server_handle(&server, &q, &r, execute, 0) == 1);
    q.status = 1;
    CHECK(sn_server_handle(&server, &q, &r, execute, 0) == -1);
    message("PASS: serial-net frame boundaries, CRC, recovery and exactly-once retry\n");
    finish(0);
}
