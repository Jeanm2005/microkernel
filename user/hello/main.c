/* A console client. It checks that it got exactly the capabilities its
 * manifest line describes, prints a line through the console server, and
 * makes sure the console won't take orders meant for the root task.
 * With HELLO_CRASH as its argument it then crashes, to show that one
 * application dying leaves the rest of the system alone. */
#include <abi/boot.h>
#include <abi/console.h>
#include <abi/selftest.h>
#include "console.h"
#include "ipc.h"
#include "ulib.h"

#define CONSOLE SLOT_USES   /* the only service in our manifest line */

int main(long arg)
{
    struct msg m = { 0 };

    /* Our manifest line provides nothing and uses only the console. */
    int extra = 0;
    for (long slot = 1; slot < 64; slot++)
        if (slot != CONSOLE && sys_recv(slot, &m) != -ERR_BADCAP)
            extra++;
    check(extra == 0, "holds only the capabilities its manifest line lists");
    check(sys_recv(CONSOLE, &m) == -ERR_PERM, "can send to the console but not receive from it");

    check(console_puts(CONSOLE, "hello from a client of the console server\n") == 0,
          "wrote a line through the console server");

    m = (struct msg){ 0 };
    m.w[0] = PROTO_SHUTDOWN;
    check(sys_call(CONSOLE, &m, 0) == 0 && m.w[0] == -ERR_PERM,
          "the console refuses a shutdown request from a client");

    if (arg == HELLO_CRASH) {
        console_puts(CONSOLE, "about to crash\n");
        *(volatile int *)0 = 1;
    }
    return failures ? 1 : 0;
}