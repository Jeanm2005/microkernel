/* Small, allocation-free Ethernet / ARP / IPv4 / ICMP stack.
 * Parse bytes explicitly: packet alignment and host byte order are irrelevant. */
#include "stack.h"
#include "ulib.h"
#include "wire.h"
#include "tcp.h"
#include "sockets.h"
void net_reset(void) { tcp_reset(); }
size_t net_poll(uint8_t *out, uint64_t now) { return tcp_poll(out, now); }
size_t net_reply(const uint8_t *in, size_t len, uint8_t *out,
                 const uint8_t mac[6], int *echo, uint64_t now)
{
    static const uint8_t broadcast[6] = {255,255,255,255,255,255};
    *echo = 0;
    if (len < 14 || len > NET_FRAME_MAX || (in[6] & 1)) return 0;
    unsigned type = be16(in + 12);
    if (!equal(in, mac, 6) && !(type == 0x806 && equal(in, broadcast, 6))) return 0;
    if (type == 0x806) {
        const uint8_t *arp = in + 14;
        if (len < 42 || be16(arp) != 1 || be16(arp + 2) != 0x800 ||
            arp[4] != 6 || arp[5] != 4 || be16(arp + 6) != 1 ||
            be32(arp + 24) != NET_IP || !equal(arp + 8, in + 6, 6)) return 0;
        memset(out, 0, 60);
        memcpy(out, in + 6, 6); memcpy(out + 6, mac, 6); put16(out + 12, type);
        uint8_t *a = out + 14;
        put16(a, 1); put16(a + 2, 0x800); a[4] = 6; a[5] = 4; put16(a + 6, 2);
        memcpy(a + 8, mac, 6); put32(a + 14, NET_IP);
        memcpy(a + 18, arp + 8, 6); memcpy(a + 24, arp + 14, 4);
        return 60;
    }
    if (type != 0x800 || len < 34) return 0;
    const uint8_t *ip = in + 14;
    unsigned hlen = (ip[0] & 15) * 4, total = be16(ip + 2);
    /* No options or reassembly. Reject all fragments, bad lengths, TTL 0,
     * non-unicast sources, wrong destinations and bad checksums. */
    uint32_t source = be32(ip + 12);
    if (ip[0] != 0x45 || total < hlen + 8 || total > len - 14 || total > NET_MTU ||
        (be16(ip + 6) & 0xbfff) || !ip[8] ||
        !source || source == 0xffffffff || (source >> 24) >= 224 ||
        be32(ip + 16) != NET_IP || checksum(ip, hlen)) return 0;
    if (ip[9] == 6) return tcp_reply(in, total, out, mac, now);
    if (ip[9] == 17) {
        const uint8_t *udp = ip + 20;
        size_t n = total - 20;
        if (!be16(udp) || be16(udp + 4) != n ||
            (be16(udp + 6) && transport_checksum(ip, n))) return 0;
        if (be16(udp + 2) != NET_ECHO_PORT) { (void)app_udp(in, n); return 0; }
        size_t size = 14 + total;
        memset(out, 0, size < 60 ? 60 : size);
        ip_header(out, in + 6, mac, source, 17, n);
        memcpy(out + 34, udp, n);
        put16(out + 34, NET_ECHO_PORT); put16(out + 36, be16(udp));
        put16(out + 40, 0);
        unsigned sum = transport_checksum(out + 14, n);
        put16(out + 40, sum ? sum : 0xffff);
        return size < 60 ? 60 : size;
    }
    if (ip[9] != 1) return 0;
    const uint8_t *icmp = ip + hlen;
    if (icmp[0] != 8 || icmp[1] != 0 || checksum(icmp, total - hlen)) return 0;
    size_t size = 14 + total;
    memset(out, 0, size < 60 ? 60 : size);
    memcpy(out, in + 6, 6); memcpy(out + 6, mac, 6); put16(out + 12, 0x800);
    uint8_t *reply = out + 14;
    memcpy(reply, ip, total);
    reply[8] = 64; put32(reply + 12, NET_IP); put32(reply + 16, source);
    reply[10] = reply[11] = 0; put16(reply + 10, checksum(reply, hlen));
    reply[hlen] = 0; reply[hlen + 2] = reply[hlen + 3] = 0;
    put16(reply + hlen + 2, checksum(reply + hlen, total - hlen));
    *echo = 1;
    return size < 60 ? 60 : size;
}
