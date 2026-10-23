/* A console client. It checks that it got exactly the capabilities its
 * manifest line describes, prints through the console server (the only
 * way it can print), and makes sure the console won't take orders meant
 * for the root task. Its argument (the last manifest column) can also
 * make it wait for typed input, or touch the serial port directly, which
 * gets it killed. */
#include <stdint.h>
#include <abi/boot.h>
#include <abi/console.h>
#include <abi/selftest.h>
#include "console.h"
#include "io.h"
#include "ipc.h"
#include "syscall.h"
#include "ulib.h"

#define CONSOLE SLOT_USES   /* the only service in our manifest line */

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Ask the console how many lines it has received, until there is one. */
static int wait_for_input(void)
{
    uint64_t start = rdtsc();
    while (rdtsc() - start < 100000000000ull) {   /* ~20 s under QEMU */
        struct msg m = { 0 };
        m.w[0] = CONSOLE_STATS;
        if (sys_call(CONSOLE, &m, 0) == 0 && m.w[0] >= 1)
            return 1;
        sys_yield();
    }
    return 0;
}

int main(long arg)
{
    printf_use_console(CONSOLE);
    struct msg m = { 0 };

    /* Our manifest line provides nothing, uses only the console and has
     * no devices. */
    int extra = 0;
    for (long slot = 1; slot < 64; slot++)
        if (slot != CONSOLE && sys_recv(slot, &m) != -ERR_BADCAP)
            extra++;
    check(extra == 0, "holds only the capabilities its manifest line lists");
    check(sys_recv(CONSOLE, &m) == -ERR_PERM, "can send to the console but not receive from it");
    check(sys_debug_write("x", 1) == -ERR_PERM,
          "printing through the kernel is refused; this line came through the console");

    check(console_puts(CONSOLE, "hello from a client of the console server\n") == 0,
          "wrote a line through the console server");

    m = (struct msg){ 0 };
    m.w[0] = PROTO_SHUTDOWN;
    check(sys_call(CONSOLE, &m, 0) == 0 && m.w[0] == -ERR_PERM,
          "the console refuses a shutdown request from a client");

    if (arg == HELLO_WAIT_INPUT) {
        printf("waiting for a line on the serial port\n");
        check(wait_for_input(), "the console received a line of input");
    }
    if (arg == HELLO_CRASH) {
        printf("about to write to the serial port directly\n");
        outb(0x3f8, '!');   /* not our port: the CPU refuses, the kernel kills us */
    }
    return failures ? 1 : 0;
}
