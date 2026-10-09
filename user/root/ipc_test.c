/* The two halves of the kernel's IPC self-test. Both run from the root
 * task image in separate processes. The kernel gave the server a receive
 * capability to an endpoint (slot TEST_SLOT_EP) and a factory (slot
 * TEST_SLOT_FACTORY); the client got only a badged send capability. */
#include <stdint.h>
#include <abi/selftest.h>
#include "ipc.h"
#include "root.h"
#include "ulib.h"

int ipc_server(long mode)
{
    struct msg m;
    long ntfn = 0, bits_seen = 0;

    long r = sys_recv(TEST_SLOT_EP, &m);
    for (;;) {
        if (r) {
            printf("FAILED: receive returned %ld\n", r);
            return 1;
        }
        long reply[4] = { 0 };
        long send_cap = 0;

        switch (m.w[0]) {
        case OP_PING:
            if (mode == ROOT_MODE_IPC_SERVER_CRASH && m.w[1] == TEST_CRASH_AT) {
                printf("crashing while handling ping %ld\n", m.w[1]);
                *(volatile int *)0 = 1;
            }
            reply[0] = m.w[1] + 1;
            reply[1] = m.badge;   /* tell the client which badge we saw */
            break;

        case OP_GET_NOTIFICATION:
            /* Make a notification with our factory and give the client a
             * copy that can only signal it; we keep the right to wait. */
            ntfn = sys_obj_create(TEST_SLOT_FACTORY, OBJ_NOTIFICATION);
            send_cap = ntfn > 0 ? sys_cap_copy(ntfn, CAP_WRITE, 0) : 0;
            if (ntfn <= 0 || send_cap <= 0) {
                printf("FAILED: creating the notification (%ld, %ld)\n", ntfn, send_cap);
                return 1;
            }
            printf("created a notification, sending a signal-only copy\n");
            break;

        case OP_WAIT_SIGNAL: {
            /* Let the client go first, then block until it signals. */
            if ((r = sys_reply(reply, 0)) != 0) {
                printf("FAILED: reply returned %ld\n", r);
                return 1;
            }
            long bits;
            if ((r = sys_wait(ntfn, &bits)) != 0) {
                printf("FAILED: wait returned %ld\n", r);
                return 1;
            }
            bits_seen = bits;
            printf("woke up from wait with bits 0x%lx\n", bits);
            r = sys_recv(TEST_SLOT_EP, &m);
            continue;
        }

        case OP_GET_BITS:
            reply[0] = bits_seen;
            break;

        case OP_QUIT:
            sys_reply(reply, 0);
            printf("quitting\n");
            return 0;

        default:
            reply[0] = -1;
        }

        r = sys_reply_recv(TEST_SLOT_EP, reply, send_cap, &m);
        if (send_cap)
            sys_cap_delete(send_cap);   /* the client has its own copy now */
    }
}

int ipc_client(long mode)
{
    struct msg m = { 0 };
    long bits;

    /* Capabilities are the only way to reach anything, and they carry
     * exactly the rights they were given. */
    check(sys_recv(TEST_SLOT_EP, &m) == -ERR_PERM,
          "recv on a send-only capability fails with -ERR_PERM");
    check(sys_call(9, &m, 0) == -ERR_BADCAP, "call on an empty slot fails with -ERR_BADCAP");
    check(sys_notify(TEST_SLOT_EP, 1) == -ERR_TYPE,
          "notify on an endpoint capability fails with -ERR_TYPE");
    check(sys_cap_copy(TEST_SLOT_EP, CAP_READ, 0) == -ERR_PERM,
          "a copy can't gain rights the original lacks");
    check(sys_cap_copy(TEST_SLOT_EP, CAP_WRITE, 7) == -ERR_PERM,
          "a badged capability can't be re-badged");
    check(sys_obj_create(TEST_SLOT_EP, OBJ_ENDPOINT) == -ERR_TYPE,
          "creating kernel objects needs a factory capability");
    long copy = sys_cap_copy(TEST_SLOT_EP, CAP_WRITE, 0);
    check(copy > 0, "a copy with fewer rights works");
    check(sys_cap_delete(copy) == 0 && sys_call(copy, &m, 0) == -ERR_BADCAP,
          "a deleted capability is gone");

    /* Round trips. The server echoes n + 1 and the badge it saw. */
    long n = mode == ROOT_MODE_IPC_CLIENT_ORPHAN ? TEST_CRASH_AT + 1 : 1000;
    uint64_t t0 = rdtsc();
    for (long i = 0; i < n; i++) {
        m.w[0] = OP_PING;
        m.w[1] = i;
        long r = sys_call(TEST_SLOT_EP, &m, 0);
        if (mode == ROOT_MODE_IPC_CLIENT_ORPHAN && i == TEST_CRASH_AT) {
            check(r == -ERR_DEAD, "call returns -ERR_DEAD when the server dies mid-call");
            return failures ? 1 : 0;
        }
        if (r != 0 || m.w[0] != i + 1 || m.w[1] != TEST_CLIENT_BADGE) {
            printf("FAILED: ping %ld: r=%ld reply=%ld badge=%ld\n", i, r, m.w[0], m.w[1]);
            return 1;
        }
    }
    uint64_t cycles = rdtsc() - t0;
    printf("ok: %ld call/reply round trips, %lu cycles each, server saw badge %d\n",
           n, cycles / n, TEST_CLIENT_BADGE);

    /* A capability travels in a reply, and a notification wakes a
     * blocked waiter in another process. */
    m.w[0] = OP_GET_NOTIFICATION;
    check(sys_call(TEST_SLOT_EP, &m, 0) == 0 && m.cap > 0,
          "received a notification capability in a reply");
    long ntfn = m.cap;
    check(sys_wait(ntfn, &bits) == -ERR_PERM, "the received copy can signal but not wait");

    m.w[0] = OP_WAIT_SIGNAL;
    check(sys_call(TEST_SLOT_EP, &m, 0) == 0, "server is now waiting on the notification");
    check(sys_notify(ntfn, 0x5) == 0, "notify(0x5)");
    m.w[0] = OP_GET_BITS;
    check(sys_call(TEST_SLOT_EP, &m, 0) == 0 && m.w[0] == 0x5,
          "server woke up and saw bits 0x5");

    m.w[0] = OP_QUIT;
    check(sys_call(TEST_SLOT_EP, &m, 0) == 0, "server acknowledged quit");
    printf("done, %d failure(s)\n", failures);
    return failures ? 1 : 0;
}