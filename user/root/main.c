/* The root task: the first user program. For now it proves that user mode
 * works; from M6 it becomes the supervisor that starts every server. */
#include <stdint.h>
#include <abi/selftest.h>
#include "syscall.h"
#include "ulib.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAILED", what);
    if (!ok)
        failures++;
}

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
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