#pragma once
#include <abi/net.h>
#include "ipc.h"
#include "ulib.h"
static inline long net_request(long ep, long op, long handle, long a, long b, struct msg *out)
{
    struct msg m = { .w = { op, handle, a, b } };
    long r = sys_call_until(ep, &m, sys_clock() + CLOCK_HZ);
    if (!r && out) *out = m;
    return r ? r : m.w[0];
}
static inline long net_write(long ep, long handle, const void *data, unsigned len)
{
    if (len > NET_CHUNK) return -ERR_INVAL;
    struct msg m = { .w = { NET_SEND | len << 8, handle } };
    if (len) memcpy(&m.w[2], data, len);
    long r = sys_call_until(ep, &m, sys_clock() + CLOCK_HZ);
    return r ? r : m.w[0];
}
