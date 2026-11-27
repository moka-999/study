/* 串口控制台测试的网络边界；真正TCP资源和连接回归另链接实际lwIP。 */
#include "net_console_fake.h"
#include <string.h>

fake_net_peer_t fake_net_peers[2];

#if defined(NET_CONSOLE_CLIENT_COUNT)
uint32_t net_console_session(unsigned slot)
{
    return slot < NET_CONSOLE_CLIENT_COUNT ? fake_net_peers[slot].session : 0U;
}
uint32_t net_console_client_dropped_bytes(unsigned slot)
{
    return slot < NET_CONSOLE_CLIENT_COUNT ? fake_net_peers[slot].dropped : 0U;
}
#else
uint32_t net_console_session(void) { return fake_net_peers[0].session; }
#endif

static fake_net_peer_t *find_peer(uint32_t session)
{
    for (unsigned i = 0U; i < 2U; ++i) {
        if (session && fake_net_peers[i].session == session) return &fake_net_peers[i];
    }
    return NULL;
}

size_t net_console_read(uint32_t session, uint8_t *data, size_t capacity)
{
    fake_net_peer_t *peer = find_peer(session);
    if (!peer) return 0U;
    size_t length = peer->input_length < capacity ? peer->input_length : capacity;
    memcpy(data, peer->input, length);
    memmove(peer->input, peer->input + length, peer->input_length - length);
    peer->input_length -= length;
    return length;
}
int net_console_write(uint32_t session, const void *data, size_t length, int drop)
{
    fake_net_peer_t *peer = find_peer(session);
    if (!peer) return -1;
    if (peer->blocked || length >= sizeof(peer->output) - peer->output_length) {
        if (drop) peer->dropped += length;
        return 0;
    }
    memcpy(peer->output + peer->output_length, data, length);
    peer->output_length += length;
    peer->output[peer->output_length] = '\0';
    return 1;
}
uint32_t net_console_dropped_bytes(void)
{
    return fake_net_peers[0].dropped + fake_net_peers[1].dropped;
}
