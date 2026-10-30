#pragma once
#include "stack.h"
#define TCP_CONNECTIONS 4
#define TCP_MSS 1460
#define TCP_RETRIES 4
#define TCP_RTO 100u
#define TCP_IDLE 3000u
void tcp_reset(void);
size_t tcp_reply(const uint8_t *frame, size_t total, uint8_t *out,
                 const uint8_t mac[6], uint64_t now);
size_t tcp_poll(uint8_t *out, uint64_t now);
