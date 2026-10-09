/* System-call dispatch. The architecture code saved the user registers
 * and calls syscall_handle() with interrupts enabled. */
#include <abi/syscall.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>

uint64_t syscall_handle(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;

    switch (nr) {
    case SYS_DEBUG_WRITE:
        return (uint64_t)process_debug_write(a0, a1);
    case SYS_EXIT:
        process_exit_current((int)a0);
    case SYS_YIELD:
        thread_yield();
        return 0;
    default:
        return (uint64_t)-ERR_NOSYS;
    }
}