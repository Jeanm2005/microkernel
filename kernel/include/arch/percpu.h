#pragma once
#include <kernel/percpu.h>

/* Point this CPU's GS base at `c`. */
void arch_percpu_init(struct cpu *c);

static inline struct cpu *this_cpu(void)
{
    struct cpu *c;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(c));
    return c;
}