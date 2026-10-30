/* Passive TCP echo service: four connections, one unacknowledged segment
 * per connection. No heap allocations and no out-of-order receive queue. */
#include "tcp.h"
#include "wire.h"
#include "sockets.h"
#define FIN 1
#define SYN 2
#define RST 4
#define PSH 8
#define ACK 16
enum state { FREE, SYN_RECEIVED, ESTABLISHED, LAST_ACK };
struct connection {
    enum state state;
    uint32_t peer_ip, receive, una, next;
    uint16_t port, service, mss, window, advertised;
    unsigned app_id;
    uint8_t peer_mac[6], mac[6];
    uint64_t deadline, idle;
    unsigned retries;
    int peer_fin;
    size_t pending_len;
    uint8_t pending[NET_FRAME_MAX];
};
static struct connection connections[TCP_CONNECTIONS];
static uint32_t serial = 0x10203040;
static unsigned poll_cursor;
static int before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
void tcp_reset(void)
{
    memset(connections, 0, sizeof connections);
    poll_cursor = 0;
}
static size_t packet(struct connection *c, uint8_t *out, uint32_t seq, unsigned flags,
                      const uint8_t *payload, size_t len, unsigned window)
{
    unsigned header = flags & SYN ? 24 : 20;
    size_t size = 34 + header + len;
    memset(out, 0, size < 60 ? 60 : size);
    ip_header(out, c->peer_mac, c->mac, c->peer_ip, 6, header + (unsigned)len);
    uint8_t *tcp = out + 34;
    put16(tcp, c->service ? c->service : NET_ECHO_PORT); put16(tcp + 2, c->port);
    put32(tcp + 4, seq); put32(tcp + 8, c->receive);
    tcp[12] = (uint8_t)((header / 4) << 4); tcp[13] = flags;
    put16(tcp + 14, window);
    if (flags & SYN) { tcp[20] = 2; tcp[21] = 4; put16(tcp + 22, TCP_MSS); }
    if (len) memcpy(tcp + header, payload, len);
    put16(tcp + 16, transport_checksum(out + 14, header + len));
    return size < 60 ? 60 : size;
}
static size_t ack_packet(struct connection *c, uint8_t *out)
{
    c->advertised = c->app_id ? app_tcp_space(c->app_id) : c->pending_len ? 0 : TCP_MSS;
    return packet(c, out, c->next, ACK, NULL, 0, c->advertised);
}
static size_t tracked(struct connection *c, uint8_t *out, unsigned flags,
                       const uint8_t *data, size_t len, uint64_t now)
{
    size_t n = packet(c, out, c->next, flags, data, len, c->app_id ? app_tcp_space(c->app_id) : flags & SYN ? TCP_MSS : 0);
    c->next += (uint32_t)len + !!(flags & SYN) + !!(flags & FIN);
    memcpy(c->pending, out, n); c->pending_len = n;
    c->deadline = now + TCP_RTO; c->retries = 0;
    if (flags & FIN) c->state = LAST_ACK;
    return n;
}
static size_t retransmit(struct connection *c, uint8_t *out)
{
    memcpy(out, c->pending, c->pending_len);
    /* ACK progress may have changed since this segment was first sent. */
    put32(out + 42, c->receive); put16(out + 50, 0);
    put16(out + 50, transport_checksum(out + 14, be16(out + 16) - 20));
    return c->pending_len;
}
size_t tcp_reply(const uint8_t *frame, size_t total, uint8_t *out,
                 const uint8_t mac[6], uint64_t now)
{
    const uint8_t *ip = frame + 14, *tcp = ip + 20;
    size_t len = total - 20;
    if (len < 20 || transport_checksum(ip, len)) return 0;
    unsigned header = (tcp[12] >> 4) * 4, flags = tcp[13];
    if (header < 20 || header > len || (tcp[12] & 15) || !be16(tcp)) return 0;
    unsigned mss = TCP_MSS;
    for (unsigned i = 20; i < header;) {
        unsigned kind = tcp[i];
        if (!kind) break;
        if (kind == 1) { i++; continue; }
        if (i + 1 >= header || tcp[i+1] < 2 || i + tcp[i+1] > header) return 0;
        if (kind == 2) {
            if (tcp[i+1] != 4 || !be16(tcp + i + 2)) return 0;
            unsigned offered = be16(tcp + i + 2);
            if (offered < mss) mss = offered;
        }
        i += tcp[i+1];
    }
    len -= header;
    const uint8_t *data = tcp + header;
    uint32_t seq = be32(tcp + 4), ack = be32(tcp + 8), peer = be32(ip + 12);
    unsigned port = be16(tcp), service = be16(tcp + 2);
    struct connection *c = NULL, *free = NULL;
    for (unsigned i = 0; i < TCP_CONNECTIONS; i++) {
        struct connection *candidate = &connections[i];
        if (candidate->state == FREE) free = candidate;
        else if (candidate->peer_ip == peer && candidate->port == port && service == candidate->service)
            c = candidate;
    }
    if (!c) {
        if (flags & RST) return 0;
        if ((service == NET_ECHO_PORT || app_listening(service)) && (flags & (SYN|ACK|FIN)) == SYN && !len) {
            if (!free) return 0; /* fixed resource budget under SYN floods */
            c = free; memset(c, 0, sizeof *c);
            c->state = SYN_RECEIVED; c->service = service; c->peer_ip = peer; c->port = (uint16_t)port;
            c->mss = (uint16_t)mss; c->window = be16(tcp + 14);
            memcpy(c->peer_mac, frame + 6, 6); memcpy(c->mac, mac, 6);
            c->receive = seq + 1; c->una = c->next = (serial += 65537) + (uint32_t)now;
            c->idle = now + TCP_IDLE;
            return tracked(c, out, SYN|ACK, NULL, 0, now);
        }
        /* Closed port / nonexistent connection: RFC reset sequence rules. */
        struct connection reset = { .peer_ip = peer, .port = (uint16_t)port };
        memcpy(reset.peer_mac, frame + 6, 6); memcpy(reset.mac, mac, 6);
        reset.receive = seq + (uint32_t)len + !!(flags & SYN) + !!(flags & FIN);
        size_t n = packet(&reset, out, flags & ACK ? ack : 0,
                          flags & ACK ? RST : RST|ACK, NULL, 0, 0);
        put16(out + 34, service); put16(out + 50, 0);
        put16(out + 50, transport_checksum(out + 14, 20));
        return n;
    }
    if (flags & RST) {
        if (seq == c->receive) { if (c->app_id) app_tcp_gone(c->app_id); memset(c, 0, sizeof *c); return 0; }
        return ack_packet(c, out); /* an inexact RST cannot destroy state */
    }
    if (c->state == SYN_RECEIVED && (flags & (SYN|ACK|FIN)) == SYN &&
        seq + 1 == c->receive && !len) return retransmit(c, out);
    if (!(flags & ACK) || (flags & SYN)) return ack_packet(c, out);
    if (before(c->next, ack) || before(ack, c->una)) return ack_packet(c, out);
    /* Reject out-of-window data before applying its ACK or window update. */
    if (before(c->receive, seq) ||
        (before(seq, c->receive) && c->receive - seq > len))
        return ack_packet(c, out);
    if (c->state == SYN_RECEIVED && (ack != c->next || seq != c->receive))
        return retransmit(c, out);
    int released = c->pending_len && ack == c->next;
    if (before(c->una, ack)) c->una = ack;
    if (released) {
        c->pending_len = 0;
        if (c->state == LAST_ACK) { if (c->app_id) app_tcp_gone(c->app_id); memset(c, 0, sizeof *c); return 0; }
        if (c->state == SYN_RECEIVED) {
            c->state = ESTABLISHED;
            if (c->service != NET_ECHO_PORT) {
                c->app_id = (unsigned)(c - connections) + 1;
                if (!app_tcp_open(c->app_id, c->service, c->peer_ip, c->port)) {
                    size_t n = packet(c, out, c->next, RST|ACK, NULL, 0, 0);
                    memset(c, 0, sizeof *c); return n;
                }
            }
        }
    }
    c->window = be16(tcp + 14); c->idle = now + TCP_IDLE;
    uint32_t skip = c->receive - seq;
    if (skip > len) return ack_packet(c, out);
    data += skip; len -= skip;
    int fin = (flags & FIN) && skip <= (uint32_t)(total - 20 - header);
    if (c->app_id) {
        unsigned accepted = c->peer_fin ? 0 : app_tcp_receive(c->app_id, data, len);
        c->receive += accepted;
        if (!c->peer_fin && fin && accepted == len) {
            c->receive++; c->peer_fin = 1; app_tcp_eof(c->app_id);
        }
        return len || fin || released || skip ? ack_packet(c, out) : 0;
    }
    if (c->peer_fin) return c->pending_len ? retransmit(c, out) : ack_packet(c, out);
    if (c->pending_len) {
        if (!len && fin) { c->receive++; c->peer_fin = 1; }
        return len || fin || skip ? ack_packet(c, out) : 0;
    }
    size_t accepted = len;
    if (accepted > c->mss) accepted = c->mss;
    if (accepted > c->window) accepted = c->window;
    c->receive += (uint32_t)accepted;
    if (fin && accepted == len) { c->receive++; c->peer_fin = 1; }
    if (accepted || c->peer_fin)
        return tracked(c, out, ACK | (accepted ? PSH : 0) | (c->peer_fin ? FIN : 0),
                       data, accepted, now);
    if (len || fin || released || skip) return ack_packet(c, out);
    return 0;
}
size_t tcp_poll(uint8_t *out, uint64_t now)
{
    for (unsigned i = 0; i < TCP_CONNECTIONS; i++) {
        struct connection *c = &connections[poll_cursor++ % TCP_CONNECTIONS];
        if (c->state == FREE) continue;
        if (now >= c->idle || (c->pending_len && now >= c->deadline && c->retries == TCP_RETRIES)) {
            size_t n = packet(c, out, c->una, RST|ACK, NULL, 0, 0);
            if (c->app_id) app_tcp_gone(c->app_id);
            memset(c, 0, sizeof *c);
            return n;
        }
        if (c->pending_len && now >= c->deadline) {
            c->deadline = now + (TCP_RTO << ++c->retries);
            return retransmit(c, out);
        }
        if (c->app_id && c->state == ESTABLISHED) {
            if (!c->pending_len) {
                uint8_t data[TCP_MSS];
                unsigned limit = c->window < c->mss ? c->window : c->mss;
                unsigned n = app_tcp_send(c->app_id, data, limit);
                if (n) return tracked(c, out, PSH|ACK, data, n, now);
                if (app_tcp_closing(c->app_id)) {
                    if (c->peer_fin) return tracked(c, out, FIN|ACK, NULL, 0, now);
                    size_t size = packet(c, out, c->next, RST|ACK, NULL, 0, 0);
                    app_tcp_gone(c->app_id); memset(c, 0, sizeof *c); return size;
                }
            }
            if (c->advertised != app_tcp_space(c->app_id)) return ack_packet(c, out);
        }
        if (!c->app_id && !c->pending_len && c->peer_fin) return tracked(c, out, FIN|ACK, NULL, 0, now);
    }
    return 0;
}
