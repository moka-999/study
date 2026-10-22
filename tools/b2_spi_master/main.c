#include "board.h"
#include "wire.h"
#include <stdio.h>
#include <string.h>

/* 本程序只用于另一块裸机主机板；不使用RTOS、不访问Flash信息区或RK。 */
static master_state_t s_desired;
static uint64_t s_sequence;
static uint8_t s_poll = 1U;
static uint8_t s_request[B2_RESPONSE_SIZE];
static uint8_t s_reply[B2_MAX_FRAME], s_ff[B2_MAX_FRAME];

/* 一写一读是两个CS事务；先13字节、再剩余字节必须保持同一次CS。 */
static int read_reply(uint64_t sequence, master_reply_t *reply)
{
    master_select(1);
    int ok = master_transfer(s_ff, s_reply, B2_HEADER_SIZE);
    size_t size = ok ? master_frame_size(s_reply) : 0U;
    if (size) ok = master_transfer(s_ff, s_reply + B2_HEADER_SIZE, size - B2_HEADER_SIZE);
    master_select(0);
    master_delay(1U);
    return ok && size && master_decode(s_reply, size, sequence, reply);
}

static void exchange(const master_state_t *state)
{
    uint64_t sequence = ++s_sequence;
    size_t length = master_build(sequence, state, s_request, sizeof(s_request));
    if (!length) { console_write("Local argument error\r\n"); return; }
    master_reply_t reply;
    for (unsigned retry = 0U; retry <= 2U; ++retry) {
        master_select(1);
        int written = master_transfer(s_request, s_reply, length);
        master_select(0);
        uint32_t started = master_millis();
        master_delay(5U);
        while (written && (uint32_t)(master_millis() - started) < 100U) {
            if (read_reply(sequence, &reply)) {
                char message[160];
                if (reply.status == 0U && reply.tester_id)
                    (void)snprintf(message, sizeof(message),
                        "seq=%lu status=0 tester=%u site=%u IP=192.168.210.%u:8888\r\n",
                        (unsigned long)sequence, reply.tester_id, reply.site_id,
                        10U * reply.tester_id + reply.site_id);
                else
                    (void)snprintf(message, sizeof(message),
                        "seq=%lu status=%u tester=%u site=%u (0=OK 1=bad 2=busy 3=unsupported 4=failed)\r\n",
                        (unsigned long)sequence, reply.status, reply.tester_id, reply.site_id);
                console_write(message);
                return;
            }
            master_delay(5U);
        }
        master_delay(1U);
    }
    /* 不把超时解释成“未执行”；周期交换仍可用同值状态重新确认。 */
    console_write("No confirmed reply after 3 writes; execution is UNKNOWN\r\n");
}

static void help(const char *args)
{
    (void)args;
    console_write(
        "\r\nquery          read IDs only\r\n"
        "id 3           set desired testerId 1..21\r\n"
        "mode 1 1       mirror 0..2, mode 0=ambient / 1=hot\r\n"
        "poll on|off    1s full-known-state exchange (default on)\r\n"
        "help           this message\r\n"
        "Modes only affect slave round LEDs; never control heaters.\r\n");
}

static void query(const char *args)
{
    if (*args) { console_write("Usage: query\r\n"); return; }
    master_state_t empty = {0};
    exchange(&empty);
}

static void set_id(const char *args)
{
    unsigned long tester;
    char extra;
    if (sscanf(args, "%lu %c", &tester, &extra) != 1 || tester < 1UL || tester > 21UL) {
        console_write("Usage: id 1..21\r\n"); return;
    }
    master_state_t update = {0};
    update.tester_id = s_desired.tester_id = (uint8_t)tester;
    exchange(&update);
}

static void set_mode(const char *args)
{
    unsigned long mirror, mode;
    char extra;
    if (sscanf(args, "%lu %lu %c", &mirror, &mode, &extra) != 2 || mirror > 2UL || mode > 1UL) {
        console_write("Usage: mode 0..2 0|1\r\n"); return;
    }
    master_state_t update = {0};
    update.thermal_mask = (uint8_t)(1U << mirror);
    update.thermal_mode[mirror] = (uint8_t)mode;
    s_desired.thermal_mask |= update.thermal_mask;
    s_desired.thermal_mode[mirror] = (uint8_t)mode;
    exchange(&update);
}

static void poll_command(const char *args)
{
    if (strcmp(args, "on") == 0) s_poll = 1U;
    else if (strcmp(args, "off") == 0) s_poll = 0U;
    else { console_write("Usage: poll on|off\r\n"); return; }
    console_write(s_poll ? "Polling ON\r\n" : "Polling OFF; slave will time out after 3s\r\n");
}

static void command(char *line)
{
    static const struct { const char *name; void (*run)(const char *); } table[] = {
        {"help", help}, {"query", query}, {"id", set_id}, {"mode", set_mode}, {"poll", poll_command}
    };
    while (*line == ' ' || *line == '\t') ++line;
    char *args = line;
    while (*args && *args != ' ' && *args != '\t') ++args;
    if (*args) *args++ = '\0';
    while (*args == ' ' || *args == '\t') ++args;
    char *end = args + strlen(args);
    while (end > args && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
    for (size_t i = 0U; i < sizeof(table) / sizeof(table[0]); ++i)
        if (strcmp(line, table[i].name) == 0) { table[i].run(args); return; }
    if (*line) console_write("Unknown command; type help\r\n");
}

int main(void)
{
    if (!master_board_init()) for (;;) { }
    memset(s_ff, 0xFF, sizeof(s_ff));
    console_write("\r\nApollo V2 bare SPI master / UART1 115200 8N1 / SPI3 1MHz\r\n");
    help("");
    char line[80];
    size_t used = 0U;
    int discard = 0;
    uint32_t last = master_millis();
    for (;;) {
        int byte = console_read();
        if (byte == -2) discard = 1;
        else if (byte == '\r' || byte == '\n') {
            if (discard) console_write("RX overflow/error: line discarded\r\n");
            else if (used) { line[used] = '\0'; command(line); }
            used = 0U; discard = 0;
        } else if (byte >= 0 && !discard) {
            if (byte == 8 || byte == 127) { if (used) --used; }
            else if (byte >= 32 && byte < 127 && used < sizeof(line) - 1U) line[used++] = (char)byte;
            else discard = 1;
        }
        if (s_poll && (uint32_t)(master_millis() - last) >= 1000U) {
            exchange(&s_desired);
            last = master_millis();
        }
    }
}
