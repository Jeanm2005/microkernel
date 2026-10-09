/* Kernel threads and the scheduler.
 *
 * Policy: 256 fixed priorities (255 = most urgent). The highest-priority
 * ready thread always runs; threads of equal priority take turns in
 * TIMESLICE_TICKS slices (round robin). A thread that becomes ready with a
 * higher priority than the running one preempts it at once. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <kernel/ipc.h>

struct process;

#define SCHED_HZ          100   /* timer ticks per second */
#define TIMESLICE_TICKS   2     /* 20 ms */

#define PRIO_IDLE         0
#define PRIO_DEFAULT      128

enum thread_state {
    THREAD_READY,      /* in a run queue */
    THREAD_RUNNING,
    THREAD_SLEEPING,   /* on the sleep list until wake_tick */
    THREAD_BLOCKED,    /* waiting on a wait queue or for an IPC reply */
    THREAD_DEAD,       /* exited; freed after we switch away from it */
};

/* A FIFO of blocked threads (linked through thread->next). */
struct waitq {
    struct thread *head, *tail;
};

struct thread {
    uint64_t          id;
    char              name[24];
    enum thread_state state;
    uint8_t           priority;
    uint64_t          saved_rsp;    /* valid while not running */
    uint64_t          kstack_top;
    struct thread    *next;         /* run-queue, sleep-list or wait-queue link */
    uint64_t          wake_tick;
    uint32_t          slice_left;   /* ticks left in the current timeslice */
    uint64_t          run_ticks;    /* ticks during which this thread was running */
    struct process   *proc;         /* owning user process; NULL for kernel threads */
    struct ipc_state  ipc;          /* IPC bookkeeping (kernel/ipc.h) */
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
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg,
                             uint8_t priority);
/* A thread of user process `proc`, created but not started: it will run
 * in proc's address space, and proc is told (process_thread_gone) once
 * the thread is reaped. Start it with thread_start(). */
struct thread *thread_new_user(const char *name, void (*fn)(void *), void *arg,
                               uint8_t priority, struct process *proc);
void thread_start(struct thread *t);
__attribute__((noreturn)) void thread_exit(void);
void thread_yield(void);
void thread_sleep(uint64_t ticks);
struct thread *thread_current(void);

uint64_t sched_ticks(void);

/* Blocking primitives. All of these need interrupts off (irq_save()). */
void waitq_push(struct waitq *q, struct thread *t);
struct thread *waitq_pop(struct waitq *q);    /* NULL if empty */
bool waitq_empty(const struct waitq *q);
/* Block the current thread (the caller has already recorded where it
 * waits) and run something else; returns once someone wakes it. */
void sched_block(void);
/* Make a blocked thread ready. It preempts the current thread at the next
 * opportunity if its priority is higher. */
void sched_wake(struct thread *t);
/* Block the current thread and hand the CPU straight to `next` (which
 * must be blocked), skipping the run queue unless a higher-priority
 * thread is waiting. The IPC fast path: a call switches directly to the
 * server, and the server's reply switches directly back. */
void sched_block_and_switch(struct thread *next);
/* How many times sched_block_and_switch() handed over directly. */
uint64_t sched_direct_switches(void);

/* Hooks for the architecture code. */
void sched_timer_tick(void);            /* from the timer interrupt */
void sched_preempt_if_needed(void);     /* at the end of every interrupt */
void thread_entry(void (*fn)(void *), void *arg);   /* first code a thread runs */