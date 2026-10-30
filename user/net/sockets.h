#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ipc.h"
void app_reset(unsigned epoch);
void app_request(const struct msg *m, long reply[4]);
int app_listening(unsigned port);
int app_tcp_open(unsigned id, unsigned port, uint32_t peer, unsigned peer_port);
unsigned app_tcp_space(unsigned id);
unsigned app_tcp_receive(unsigned id, const uint8_t *p, unsigned len);
unsigned app_tcp_send(unsigned id, uint8_t *p, unsigned limit);
void app_tcp_eof(unsigned id);
void app_tcp_gone(unsigned id);
int app_tcp_closing(unsigned id);
int app_udp(const uint8_t *frame, size_t len);
size_t app_poll(uint8_t *out, const uint8_t mac[6]);
