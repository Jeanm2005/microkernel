#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ulib.h"
static inline unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static inline uint32_t be32(const uint8_t *p) { return (uint32_t)be16(p) << 16 | be16(p + 2); }
static inline void put16(uint8_t *p, unsigned n) { p[0] = n >> 8; p[1] = n; }
static inline void put32(uint8_t *p, uint32_t n) { put16(p, n >> 16); put16(p + 2, n); }
static inline int equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static inline unsigned word_sum(const uint8_t *p, size_t len)
{
    unsigned sum = 0;
    while (len >= 2) { sum += be16(p); p += 2; len -= 2; }
    if (len) sum += (unsigned)*p << 8;
    return sum;
}
static inline unsigned folded(unsigned sum)
{
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (~sum) & 0xffff;
}
static inline unsigned checksum(const uint8_t *p, size_t len) { return folded(word_sum(p, len)); }
static inline unsigned transport_checksum(const uint8_t *ip, size_t len)
{
    return folded(word_sum(ip + 12, 8) + ip[9] + (unsigned)len + word_sum(ip + 20, len));
}
static inline void ip_header(uint8_t *out, const uint8_t peer_mac[6], const uint8_t mac[6],
                             uint32_t peer_ip, unsigned protocol, unsigned len)
{
    memset(out, 0, 34);
    memcpy(out, peer_mac, 6); memcpy(out + 6, mac, 6); put16(out + 12, 0x800);
    uint8_t *ip = out + 14;
    ip[0] = 0x45; put16(ip + 2, 20 + len); ip[8] = 64; ip[9] = protocol;
    put32(ip + 12, NET_IP); put32(ip + 16, peer_ip); put16(ip + 10, checksum(ip, 20));
}
