#include <abi/net.h>
#include "stack.h"
#include "wire.h"
#include "sockets.h"
#define SLOTS 8
#define BUFFER 256
enum { FREE, OPEN, LISTEN, ACCEPTED, STREAM, CLOSING, DEAD };
struct socket {
    unsigned state, type, port, conn, rx_len, tx_len, serial;
    uint64_t owner, handle, parent;
    uint32_t peer;
    unsigned peer_port;
    uint8_t peer_mac[6], rx[BUFFER], tx[BUFFER];
    int eof, datagram, tx_datagram, error;
};
static struct socket slots[SLOTS];
static unsigned epoch;
void app_reset(unsigned generation) { memset(slots, 0, sizeof slots); epoch = generation; }
static struct socket *allocate(uint64_t owner)
{
    unsigned used = 0;
    for (unsigned i = 0; i < SLOTS; i++) if (slots[i].state && slots[i].owner == owner) used++;
    if (used >= 4) return NULL;
    for (unsigned i = 0; i < SLOTS; i++) {
        struct socket *s = &slots[i];
        if (s->state || s->serial == 65535) continue;
        unsigned serial = s->serial + 1;
        memset(s, 0, sizeof *s); s->serial = serial; s->owner = owner; s->state = OPEN;
        s->handle = (uint64_t)epoch << 32 | serial << 8 | (i + 1);
        return s;
    }
    return NULL;
}
static struct socket *connection(unsigned id)
{
    for (unsigned i = 0; i < SLOTS; i++) if (slots[i].state && slots[i].conn == id) return &slots[i];
    return NULL;
}
int app_listening(unsigned port)
{
    for (unsigned i = 0; i < SLOTS; i++) if (slots[i].state == LISTEN && slots[i].port == port) return 1;
    return 0;
}
int app_tcp_open(unsigned id, unsigned port, uint32_t peer, unsigned peer_port)
{
    for (unsigned i = 0; i < SLOTS; i++) {
        struct socket *parent = &slots[i];
        if (parent->state != LISTEN || parent->port != port) continue;
        struct socket *s = allocate(parent->owner);
        if (!s) return 0;
        s->state = ACCEPTED; s->type = NET_TCP; s->parent = parent->handle;
        s->conn = id; s->port = port; s->peer = peer; s->peer_port = peer_port;
        return 1;
    }
    return 0;
}
unsigned app_tcp_space(unsigned id)
{
    struct socket *s = connection(id);
    return s && s->state != CLOSING && s->state != DEAD ? BUFFER - s->rx_len : 0;
}
unsigned app_tcp_receive(unsigned id, const uint8_t *p, unsigned len)
{
    struct socket *s = connection(id);
    unsigned count = app_tcp_space(id);
    if (count > len) count = len;
    if (count) { memcpy(s->rx + s->rx_len, p, count); s->rx_len += count; }
    return count;
}
unsigned app_tcp_send(unsigned id, uint8_t *p, unsigned limit)
{
    struct socket *s = connection(id);
    if (!s) return 0;
    unsigned n = s->tx_len < limit ? s->tx_len : limit;
    memcpy(p, s->tx, n);
    for (unsigned i = n; i < s->tx_len; i++) s->tx[i-n] = s->tx[i];
    s->tx_len -= n;
    return n;
}
void app_tcp_eof(unsigned id) { struct socket *s = connection(id); if (s) s->eof = 1; }
int app_tcp_closing(unsigned id)
{
    struct socket *s = connection(id);
    return !s || (s->state == CLOSING && !s->tx_len);
}
void app_tcp_gone(unsigned id)
{
    struct socket *s = connection(id);
    if (s) { s->conn = 0; s->eof = 1; s->error = 1; s->state = (s->state == CLOSING || s->state == ACCEPTED) ? FREE : DEAD; }
}
int app_udp(const uint8_t *frame, size_t len)
{
    const uint8_t *udp = frame + 34;
    for (unsigned i = 0; i < SLOTS; i++) {
        struct socket *s = &slots[i];
        if (s->state != OPEN || s->type != NET_UDP || s->port != be16(udp + 2)) continue;
        if (s->datagram || s->tx_datagram || len - 8 > BUFFER) return 1;
        s->rx_len = len - 8; memcpy(s->rx, udp + 8, s->rx_len); s->datagram = 1;
        s->peer = be32(frame + 26); s->peer_port = be16(udp); memcpy(s->peer_mac, frame + 6, 6);
        return 1;
    }
    return 0;
}
size_t app_poll(uint8_t *out, const uint8_t mac[6])
{
    for (unsigned i = 0; i < SLOTS; i++) {
        struct socket *s = &slots[i];
        if (!s->state || s->type != NET_UDP || !s->tx_datagram) continue;
        size_t n = s->tx_len;
        memset(out, 0, 60); ip_header(out, s->peer_mac, mac, s->peer, 17, n + 8);
        put16(out + 34, s->port); put16(out + 36, s->peer_port); put16(out + 38, n + 8);
        put16(out + 40, 0); memcpy(out + 42, s->tx, n);
        unsigned checksum = transport_checksum(out + 14, n + 8);
        put16(out + 40, checksum ? checksum : 0xffff); s->tx_len = 0; s->tx_datagram = 0;
        return n + 42 < 60 ? 60 : n + 42;
    }
    return 0;
}
void app_request(const struct msg *m, long out[4])
{
    memset(out, 0, 4 * sizeof(long));
    unsigned op = PROTO_OP(m->w[0]), n = (unsigned long)m->w[0] >> 8;
    if (op == NET_INFO) { out[0] = epoch; return; }
    if (op == NET_OPEN) {
        if (m->w[1] != NET_UDP && m->w[1] != NET_TCP) { out[0] = -ERR_INVAL; return; }
        struct socket *s = allocate(m->badge);
        if (!s) { out[0] = -ERR_FULL; return; }
        s->type = m->w[1]; out[0] = (long)s->handle; return;
    }
    if ((uint64_t)m->w[1] >> 32 != epoch) { out[0] = -ERR_STALE; return; }
    struct socket *s = NULL;
    for (unsigned i = 0; i < SLOTS; i++) if (slots[i].state && slots[i].handle == (uint64_t)m->w[1]) s = &slots[i];
    if (!s) { out[0] = -ERR_BADCAP; return; }
    if (s->owner != (uint64_t)m->badge) { out[0] = -ERR_PERM; return; }
    switch (op) {
    case NET_BIND:
        if (s->state != OPEN || s->port || m->w[2] < 1024 || m->w[2] > 65535) { out[0] = -ERR_INVAL; break; }
        for (unsigned i = 0; i < SLOTS; i++)
            if (slots[i].state && slots[i].port == (unsigned)m->w[2] && slots[i].type == s->type) {
                out[0] = -ERR_EXIST; return;
            }
        s->port = m->w[2]; break;
    case NET_LISTEN:
        if (s->type != NET_TCP || s->state != OPEN || !s->port) out[0] = -ERR_INVAL;
        else s->state = LISTEN;
        break;
    case NET_ACCEPT:
        out[0] = -ERR_AGAIN;
        if (s->state != LISTEN) { out[0] = -ERR_INVAL; break; }
        for (unsigned i = 0; i < SLOTS; i++)
            if (slots[i].state == ACCEPTED && slots[i].parent == s->handle) {
                slots[i].state = STREAM; out[0] = slots[i].handle;
                out[1] = slots[i].peer; out[2] = slots[i].peer_port; break;
            }
        break;
    case NET_RECV: {
        if (n > NET_CHUNK || !n || s->state == LISTEN) { out[0] = -ERR_INVAL; break; }
        if (!s->rx_len && !s->datagram) { out[0] = s->error ? -ERR_IO : s->eof ? 0 : -ERR_AGAIN; break; }
        if (n > s->rx_len) n = s->rx_len;
        memcpy(&out[2], s->rx, n);
        for (unsigned i = n; i < s->rx_len; i++) s->rx[i-n] = s->rx[i];
        s->rx_len -= n; if (!s->rx_len) s->datagram = 0;
        out[0] = n; out[1] = s->rx_len; break;
    }
    case NET_SEND:
        if (n > NET_CHUNK || !s->port || s->state == LISTEN || s->state == DEAD || s->state == CLOSING) {
            out[0] = -ERR_INVAL; break;
        }
        if (s->type == NET_UDP && (!s->peer_port || s->tx_datagram)) { out[0] = -ERR_AGAIN; break; }
        if (s->tx_len + n > BUFFER) { out[0] = -ERR_AGAIN; break; }
        memcpy(s->tx + s->tx_len, &m->w[2], n); s->tx_len += n; if (s->type == NET_UDP) s->tx_datagram = 1; out[0] = n; break;
    case NET_CLOSE:
        if (s->conn) s->state = CLOSING;
        else {
            if (s->state == LISTEN) for (unsigned i = 0; i < SLOTS; i++)
                if (slots[i].state == ACCEPTED && slots[i].parent == s->handle) slots[i].state = CLOSING;
            s->state = FREE;
        }
        break;
    default: out[0] = -ERR_NOSYS;
    }
}
