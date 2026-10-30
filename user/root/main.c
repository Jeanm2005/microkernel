/* The root task: the first user program. Started in ROOT_MODE_INIT it
 * starts the system from a manifest (init.c); the other modes are the
 * kernel's user-mode and IPC self-tests. */
#include <stdint.h>
#include <abi/selftest.h>
#include "root.h"
#include "syscall.h"
#include "ipc.h"
#include "ulib.h"

uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));   /* allowed in ring 3 */
    return ((uint64_t)hi << 32) | lo;
}

/* Compute for a while without any system call. The only way anything else
 * runs meanwhile is the timer interrupting us in ring 3, the kernel
 * switching threads, and later resuming us exactly where we were. */
static void busy_wait(void)
{
    uint64_t start = rdtsc();
    while (rdtsc() - start < 1000000000ull)
        ;
    printf("ran 10^9 cycles without a system call\n");
}

/* Deliberate crimes, one per test mode. Each must get this process killed
 * without disturbing the kernel. */
static void misbehave(long mode)
{
    switch (mode) {
    case ROOT_MODE_PAGEFAULT:
        printf("writing through a null pointer\n");
        *(volatile int *)0 = 1;
        break;
    case ROOT_MODE_PRIVILEGED:
        printf("executing cli (ring 0 only)\n");
        __asm__ volatile("cli");
        break;
    case ROOT_MODE_KERNEL_READ:
        printf("reading kernel memory\n");
        (void)*(volatile uint64_t *)0xffffffff80000000ull;
        break;
    }
}

int main(long mode)
{
    if (mode == ROOT_MODE_REVOKED) {
        struct msg message = { .w = { 55 } };
        check(sys_call(TEST_SLOT_EP, &message, 0) == -ERR_REVOKED,
              "revocation cancels a blocked call across processes");
        check(sys_cap_delete(TEST_SLOT_EP) == 0, "revoked capabilities remain deletable");
        return failures ? 1 : 0;
    }
    if (mode == ROOT_MODE_INIT || mode == ROOT_MODE_INIT_CRASH || mode == ROOT_MODE_INIT_INPUT ||
        mode == ROOT_MODE_INIT_RESTART || mode == ROOT_MODE_INIT_TIMEOUT ||
        mode == ROOT_MODE_INIT_REPEATED || mode == ROOT_MODE_INIT_EXHAUST ||
        mode == ROOT_MODE_INIT_BOOT_TIMEOUT || mode == ROOT_MODE_INIT_NOISY_TIMEOUT ||
        mode == ROOT_MODE_INIT_NET || mode == ROOT_MODE_NET_SERVE || mode == ROOT_MODE_WATCHDOG || mode == ROOT_MODE_SOCKETS ||
        mode == ROOT_MODE_NET_RESTART)
        return init_main(mode);
    if (mode == ROOT_MODE_IPC_SERVER || mode == ROOT_MODE_IPC_SERVER_CRASH)
        return ipc_server(mode);
    if (mode == ROOT_MODE_IPC_CLIENT || mode == ROOT_MODE_IPC_CLIENT_ORPHAN)
        return ipc_client(mode);

    printf("hello from user space (mode %ld)\n", mode);

    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    printf("running in ring %u (cs = %x)\n", cs & 3, cs);
    check((cs & 3) == 3, "CPU privilege level is 3");

    /* The kernel must refuse pointers we don't own instead of crashing. */
    check(sys_debug_write((const void *)0xffffffff80000000ull, 8) == -ERR_FAULT,
          "debug_write(kernel address) returns -ERR_FAULT");
    check(sys_debug_write((const void *)0x1000, 8) == -ERR_FAULT,
          "debug_write(unmapped address) returns -ERR_FAULT");
    check(sys_debug_write("", DEBUG_WRITE_MAX + 1) == -ERR_INVAL,
          "debug_write(too long) returns -ERR_INVAL");
    check(syscall2(999, 0, 0) == -ERR_NOSYS, "unknown syscall returns -ERR_NOSYS");
    check(sys_yield() == 0, "yield returns 0");

    /* Stack and .bss work. */
    static volatile int bss_value;
    volatile int stack_value = 41;
    bss_value = stack_value + 1;
    check(bss_value == 42, "stack and .bss are writable");

    if (mode == ROOT_MODE_NORMAL)
        busy_wait();
    misbehave(mode);

    printf("done, %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
