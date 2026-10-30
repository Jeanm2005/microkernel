/* Native sanitizer tests of hostile packet lengths and virtual TCP time. */
#include <assert.h>
#include <stdlib.h>
#include "stack.h"
#include "tcp.h"
#include "wire.h"
#include "sockets.h"
#include <abi/net.h>
static const uint8_t mac[6] = {0x52,0x54,0,0x12,0x34,0x56};
static const uint8_t peer[6] = {0x52,0x54,0,0xab,0xcd,0xef};
static uint32_t random_state = 42;
static uint32_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
static void syn(uint8_t *packet)
{
    memset(packet, 0, NET_FRAME_MAX);
    memcpy(packet, mac, 6); memcpy(packet + 6, peer, 6); put16(packet + 12, 0x800);
    uint8_t *ip = packet + 14, *tcp = packet + 34;
    ip[0] = 0x45; put16(ip + 2, 40); ip[8] = 64; ip[9] = 6;
    put32(ip + 12, 0x0a000202); put32(ip + 16, NET_IP);
    put16(ip + 10, checksum(ip, 20));
    put16(tcp, 31000); put16(tcp + 2, NET_ECHO_PORT); put32(tcp + 4, 1000);
    tcp[12] = 5 << 4; tcp[13] = 2; put16(tcp + 14, 4096);
    put16(tcp + 16, transport_checksum(ip, 20));
}
static void sockets_test(void)
{
    app_reset(1);
    struct msg m = { .w = { NET_OPEN, NET_UDP }, .badge = 42 };
    long response[4], handles[4];
    for (unsigned i = 0; i < 4; i++) {
        app_request(&m, response); assert(response[0] > 0); handles[i] = response[0];
    }
    app_request(&m, response); assert(response[0] == -ERR_FULL);
    m = (struct msg){ .w = { NET_BIND, handles[0], 9000 }, .badge = 42 };
    app_request(&m, response); assert(!response[0]);
    m.w[1] = handles[1]; app_request(&m, response); assert(response[0] == -ERR_EXIST);
    m = (struct msg){ .w = { NET_RECV | 16 << 8, handles[0] }, .badge = 43 };
    app_request(&m, response); assert(response[0] == -ERR_PERM);
    m.badge = 42; app_request(&m, response); assert(response[0] == -ERR_AGAIN);
    app_reset(2); app_request(&m, response); assert(response[0] == -ERR_STALE);
    m = (struct msg){ .w = { NET_OPEN, NET_UDP }, .badge = 42 };
    app_request(&m, response); long handle = response[0];
    m = (struct msg){ .w = { NET_CLOSE, handle }, .badge = 42 };
    app_request(&m, response); assert(!response[0]);
    m.w[0] = NET_RECV | 16 << 8; app_request(&m, response); assert(response[0] == -ERR_BADCAP);
    app_reset(0);
}
int main(void)
{
    sockets_test();
    uint8_t seed[NET_FRAME_MAX], out[NET_FRAME_MAX];
    int echo;
    syn(seed); net_reset();
    assert(net_reply(seed, 54, out, mac, &echo, 0) == 60);
    assert(!net_poll(out, TCP_RTO - 1));
    uint64_t time = TCP_RTO;
    for (unsigned retry = 0; retry < TCP_RETRIES; retry++) {
        assert(net_poll(out, time) && out[47] == 18);
        time += TCP_RTO << (retry + 1);
    }
    assert(net_poll(out, time) && out[47] == 20);
    assert(!net_poll(out, time + TCP_IDLE));
    /* Idle and retransmission expiration must reclaim the fixed table. */
    assert(net_reply(seed, 54, out, mac, &echo, time + 1));
    /* Finish a handshake, then expire an otherwise idle established peer. */
    net_reset(); syn(seed);
    assert(net_reply(seed, 54, out, mac, &echo, 0));
    uint32_t server = be32(out + 38);
    put32(seed + 38, 1001); put32(seed + 42, server + 1); seed[47] = 16;
    put16(seed + 50, 0); put16(seed + 50, transport_checksum(seed + 14, 20));
    assert(net_reply(seed, 54, out, mac, &echo, 1));
    assert(net_poll(out, TCP_IDLE + 2) && out[47] == 20);
    assert(!net_poll(out, 2 * TCP_IDLE));
    syn(seed); net_reset();
    for (size_t n = 0; n <= NET_FRAME_MAX + 32; n++) {
        uint8_t *packet = malloc(n ? n : 1);
        assert(packet);
        memset(packet, 0, n);
        memcpy(packet, seed, n < sizeof seed ? n : sizeof seed);
        size_t result = net_reply(packet, n, out, mac, &echo, time);
        assert(!result || (result >= 60 && result <= NET_FRAME_MAX));
        free(packet);
    }
    for (unsigned trial = 0; trial < 20000; trial++) {
        size_t n = 54 + next_random() % (NET_FRAME_MAX - 53);
        uint8_t *packet = malloc(n);
        assert(packet);
        memcpy(packet, seed, 54);
        for (size_t i = 54; i < n; i++) packet[i] = (uint8_t)next_random();
        for (unsigned j = 0; j < 5; j++) packet[next_random() % n] ^= (uint8_t)next_random();
        /* Repair checksums on half the mutations to reach TCP parsing. */
        if (trial & 1) {
            packet[14] = 0x45; put16(packet + 16, (unsigned)n - 14);
            put16(packet + 24, 0); put16(packet + 24, checksum(packet + 14, 20));
            put16(packet + 50, 0);
            put16(packet + 50, transport_checksum(packet + 14, n - 34));
        }
        size_t result = net_reply(packet, n, out, mac, &echo, trial);
        assert(!result || (result >= 60 && result <= NET_FRAME_MAX));
        free(packet);
        if (!(trial % 32)) { net_reset(); (void)net_poll(out, trial + TCP_IDLE); }
    }
    return 0;
}
