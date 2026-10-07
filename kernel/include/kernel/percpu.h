/* Per-CPU state. There is one CPU for now, but everything that would
 * differ between CPUs lives here from the start, reached through
 * this_cpu() (the GS segment base on x86_64), so SMP later doesn't mean
 * hunting down globals. */
 #pragma once
 #include <stdbool.h>
 #include <stdint.h>

 struct thread;

 struct cpu {
    struct cpu *self;          /* must be first: this_cpu() reads it */
    struct thread *current;    /* running thread */
    struct thread *idle;       /* runs when nothing else can */
    struct thread *prev;       /* thread we just switched away from */
    bool need_resched;         /* set by interrupts, acted on before returning */
    uint64_t ticks;            /* timer ticks since the scheduler started */
    uint64_t switches;         /* context switches */
    uint64_t preemptions;      /* switches forced by the timer or a wakeup */
 };