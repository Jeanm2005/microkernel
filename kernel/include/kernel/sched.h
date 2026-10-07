/* Kernel threads and the scheduler.
 *
 * Policy: 256 fixed priorities (255 = most urgent). The highest-priority
 * ready thread always runs; threads of equal priority take turns in
 * TIMESLICE_TICKS slices (round robin). A thread that becomes ready with a
 * higher priority than the running one preempts it at once. */
 #pragma once
 #include <stdbool.h>
 #include <stdint.h>

 #define SCHED_HZ 100 /* timer ticks per second */
 #define TIMESLICE_TICKS 2 /* 20 ms */

 #define PRIO_IDLE 0
 #define PRIO_DEFAULT 128

 enum thread_state {
    THREAD_READY, /* in a run queue */
    THREAD_RUNNING,
    THREAD_SLEEPING, /* on the sleep list until wake_tick */
    THREAD_DEAD, /* exited; freed after we switch away from it */
 };

 struct thread {
    uint64_t id;
    char name[24];
    enum thread_state state;
    uint8_t priority;
    uint64_t saved_rsp;  /* valid while not running */
    uint64_t kstack_top;
    struct thread *next; /* run-queue or sleep-list link */
    uint64_t wake_tick;
    uint32_t slice_left; /* ticks left in the current timeslice */
    uint64_t run_ticks;  /* ticks during which this thread was running */
 };


/* Set up per-CPU state. Call early: every interrupt ends in
 * sched_preempt_if_needed(), which reads it (and does nothing until
 * sched_start() has run). Needs the slab allocator, so after paging_init(). */
 void sched_init(void);

 /* Turn the boot flow into the first thread: create the idle thread and an
 * `init` thread running fn(arg), then switch to `init`. The boot stack
 * (Limine's) is abandoned and never used again. */
 __attribute__((noreturn))
 void sched_start(void (*fn)(void *), void *arg);

 /* Create a ready thread. Returns NULL if out of memory. If its priority is
 * higher than the caller's, it runs before this returns. */
 struct thread *thread_create(const char *name, void (*fn)(void *), void *arg, uint8_t priority);
 __attribute__((noreturn)) void thread_exit(void);
void thread_yield(void);
void thread_sleep(uint64_t ticks);
struct thread *thread_current(void);

uint64_t sched_ticks(void);

/* Hooks for the architecture code. */
void sched_timer_tick(void);            /* from the timer interrupt */
void sched_preempt_if_needed(void);     /* at the end of every interrupt */
void thread_entry(void (*fn)(void *), void *arg);   /* first code a thread runs */