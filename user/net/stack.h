#pragma once
#include <stddef.h>
#include <stdint.h>
#define NET_ECHO_PORT 7
#define NET_MTU 1500
#define NET_FRAME_MAX (14 + NET_MTU)
/* IPv4 address in network byte order, represented as a numeric constant. */
#define NET_IP 0x0a00020fu /* 10.0.2.15 */
/* Build an ARP or ICMP echo reply. Returns its length, or 0 to drop.
 * Input and output must be distinct buffers of NET_FRAME_MAX bytes. */
size_t net_reply(const uint8_t *in, size_t len, uint8_t *out,
                 const uint8_t mac[6], int *echo, uint64_t now);
void net_reset(void);
size_t net_poll(uint8_t *out, uint64_t now);
