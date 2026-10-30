/* Version 1, passive guest sockets. Requests: w0=op|(length<<8), w1=handle,
 * w2..3=16 byte payload. BIND: w2=port; OPEN: w1=type. All replies w0=result.
 * RECV: w0=count, w1=bytes remaining, w2..3=bytes; 0=stream EOF.
 * ACCEPT: w0=new handle, w1=peer IPv4, w2=peer port. Operations are nonblocking
 * (-ERR_AGAIN); clients use absolute-deadline IPC and bounded retry loops.
 * UDP SEND replies to its last received peer; UDP receive chunks preserve
 * datagram boundaries via w1. TCP is passive LISTEN/ACCEPT, no active CONNECT.
 * Handles are scoped to the caller's endpoint badge and server incarnation. */
#pragma once
#include <abi/proto.h>
#define NET_INFO 1
#define NET_OPEN 2
#define NET_BIND 3
#define NET_LISTEN 4
#define NET_ACCEPT 5
#define NET_SEND 6
#define NET_RECV 7
#define NET_CLOSE 8
#define NET_UDP 1
#define NET_TCP 2
#define NET_CHUNK 16
#define NET_ARG_STALL (1ull << 32)
#define NET_TEST_STALL 0xf0 /* enabled only by supervisor test argument */
