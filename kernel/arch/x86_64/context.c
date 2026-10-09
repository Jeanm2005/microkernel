#include <stddef.h>
#include <arch/context.h>
#include <arch/cpu.h>
#include <arch/percpu.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include "gdt.h"

#define MSR_GS_BASE        0xc0000101
#define MSR_KERNEL_GS_BASE 0xc0000102   /* swapped with GS base by `swapgs` */

/* syscall_entry.S hard-codes these offsets. */
_Static_assert(offsetof(struct cpu, self) == 0, "cpu.self offset");
_Static_assert(offsetof(struct cpu, user_rsp) == 8, "cpu.user_rsp offset");
_Static_assert(offsetof(struct cpu, kernel_rsp) == 16, "cpu.kernel_rsp offset");

extern char thread_trampoline[];

uint64_t arch_context_init(uint64_t stack_top, void (*fn)(void *), void *arg)
{
    kassert(IS_ALIGNED(stack_top, 16));
    /* Build the frame arch_context_switch() pops, lowest address first:
     * r15, r14, r13, r12, rbx, rbp, return address. After the pops and
     * `ret`, RSP == stack_top, which is 16-byte aligned: exactly what the
     * C ABI wants at the `call thread_entry` in the trampoline. */
    uint64_t *sp = (uint64_t *)stack_top;
    *--sp = (uint64_t)thread_trampoline;   /* return address */
    *--sp = 0;                             /* rbp */
    *--sp = 0;                             /* rbx */
    *--sp = (uint64_t)fn;                  /* r12 */
    *--sp = (uint64_t)arg;                 /* r13 */
    *--sp = 0;                             /* r14 */
    *--sp = 0;                             /* r15 */
    return (uint64_t)sp;
}

void arch_set_kernel_stack(uint64_t stack_top)
{
    tss_set_kernel_stack(stack_top);      /* for interrupts from ring 3 */
    this_cpu()->kernel_rsp = stack_top;   /* for syscall_entry */
}

/* In the kernel, GS base points at our struct cpu. User code gets its own
 * GS base (0 for now). Every entry from ring 3 (interrupt or syscall) does
 * `swapgs`, which exchanges GS base with KERNEL_GS_BASE, and every return
 * to ring 3 swaps back, so user code never sees the kernel pointer. */
void arch_percpu_init(struct cpu *c)
{
    c->self = c;
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}