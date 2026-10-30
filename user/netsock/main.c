#include <abi/boot.h>
#include "net.h"
#include "syscall.h"
#define NET (SLOT_USES + 1)
static long retry(long op, long handle, long arg, struct msg *m)
{
    unsigned long deadline = sys_clock() + 8 * CLOCK_HZ;
    long r;
    do {
        r = net_request(NET, op, handle, arg, 0, m);
        if (r != -ERR_AGAIN) return r;
        sys_yield();
    } while (sys_clock() < deadline);
    return -ERR_TIMEDOUT;
}
int main(long restart)
{
    printf_use_console(SLOT_USES);
    long udp = net_request(NET, NET_OPEN, NET_UDP, 0, 0, 0);
    if (udp < 0) return 1;
    check(net_request(NET + 1, NET_RECV | 16 << 8, udp, 0, 0, 0) == -ERR_PERM,
          "socket handles are isolated by client badge");
    if (restart) {
        struct msg m = { .w = { NET_TEST_STALL } };
        check(sys_call_until(NET, &m, sys_clock() + CLOCK_HZ) == -ERR_TIMEDOUT,
              "a call times out while the server is blocked in another syscall");
        check(sys_proc_stop(SLOT_EVENTS) == 0, "supervisor capability can stop a server blocked in IPC");
        unsigned long end = sys_clock() + 3 * CLOCK_HZ;
        long epoch;
        do { epoch = net_request(NET, NET_INFO, 0, 0, 0, 0); } while (epoch != 2 && sys_clock() < end);
        check(epoch == 2, "reconnected to network incarnation 2");
        check(net_request(NET, NET_RECV | 16 << 8, udp, 0, 0, 0) == -ERR_STALE,
              "old socket handles cannot alias the restarted server");
        udp = net_request(NET, NET_OPEN, NET_UDP, 0, 0, 0);
    }
    long listener = net_request(NET, NET_OPEN, NET_TCP, 0, 0, 0);
    if (udp < 0 || listener < 0 || net_request(NET, NET_BIND, udp, 9000, 0, 0) ||
        net_request(NET, NET_BIND, listener, 9001, 0, 0) || net_request(NET, NET_LISTEN, listener, 0, 0, 0)) return 1;
    printf("ready: guest UDP 9000 and TCP 9001\n");
    struct msg m;
    long n = retry(NET_RECV | 16 << 8, udp, 0, &m);
    if (n <= 0 || net_write(NET, udp, &m.w[2], n) != n) return 1;
    long stream = retry(NET_ACCEPT, listener, 0, &m);
    if (stream < 0) return 1;
    unsigned total = 0;
    for (;;) {
        n = retry(NET_RECV | 16 << 8, stream, 0, &m);
        if (!n) break;
        if (n < 0 || net_write(NET, stream, &m.w[2], n) != n) return 1;
        total += n;
    }
    check(total > 256, "guest TCP client received and returned a multi-chunk stream");
    if (net_request(NET, NET_CLOSE, stream, 0, 0, 0)) return 1;
    unsigned long end = sys_clock() + 2 * CLOCK_HZ;
    int closed = 0;
    while (sys_clock() < end) {
        long r = net_request(NET, NET_RECV | 16 << 8, stream, 0, 0, 0);
        if (r == -ERR_BADCAP) { closed = 1; break; }
        sys_yield();
    }
    check(closed, "guest TCP close completed");
    net_request(NET, NET_CLOSE, udp, 0, 0, 0);
    net_request(NET, NET_CLOSE, listener, 0, 0, 0);
    printf("done, %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
