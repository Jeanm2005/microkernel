#include <arch/context.h>
#include <arch/cpu.h>
#include <arch/percpu.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include "gdt.h"

#define MSR_GS_BASE 0xc0000101

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
    tss_set_kernel_stack(stack_top);
}

void arch_percpu_init(struct cpu *c)
{
    c->self = c;
    /* TODO(M4): once user mode exists, user code must not see this, so
     * trap entry from ring 3 will `swapgs` between user and kernel GS. */
    wrmsr(MSR_GS_BASE, (uint64_t)c);
}