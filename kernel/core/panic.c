#include <arch/backtrace.h>
#include <arch/cpu.h>
#include <arch/qemu.h>
#include <kernel/kprintf.h>
#include <kernel/panic.h>

void panic_at(const char *file, int line, const char *fmt, ...)
{
    cpu_cli();
    kprintf("\n*** KERNEL PANIC at %s:%d\n*** ", file, line);
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n  panic backtrace:\n");
    backtrace_print((uint64_t)__builtin_frame_address(0));

    /* Under `make test`, end QEMU with a failure status so the test runner
     * doesn't wait for a timeout. Elsewhere this does nothing. */
    qemu_exit(1);
    cpu_halt_forever();
}