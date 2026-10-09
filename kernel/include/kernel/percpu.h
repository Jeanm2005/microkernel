/* Per-CPU state. There is one CPU for now, but everything that would
 * differ between CPUs lives here from the start, reached through
 * this_cpu() (the GS segment base on x86_64), so SMP later doesn't mean
 * hunting down globals. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct thread;

struct cpu {
    /* The first three fields are read by assembly at fixed offsets
     * (arch/x86_64/syscall_entry.S); context.c checks them at compile time. */
    struct cpu    *self;          /* offset 0: this_cpu() reads it */
    uint64_t       user_rsp;      /* offset 8: scratch for the syscall entry path */
    uint64_t       kernel_rsp;    /* offset 16: top of the running thread's kernel stack */

    struct thread *current;       /* running thread */
    struct thread *idle;          /* runs when nothing else can */
    struct thread *prev;          /* thread we just switched away from */
    uint64_t       active_root;   /* page-table root currently loaded */
    bool           need_resched;  /* set by interrupts, acted on before returning */
    uint64_t       ticks;         /* timer ticks since the scheduler started */
    uint64_t       switches;      /* context switches */
    uint64_t       preemptions;   /* switches forced by the timer or a wakeup */
    uint64_t       direct_switches; /* IPC handoffs that skipped the run queue */
};