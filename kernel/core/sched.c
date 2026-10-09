/* Threads and the scheduler.
 *
 * Locking: there is one CPU, so "interrupts disabled" is the lock. Every
 * function that touches run queues or thread state does it between
 * irq_save() and irq_restore(). SMP will replace this with real locks.
 *
 * Run queues: one FIFO list per priority, plus a 256-bit bitmap of which
 * lists are non-empty, so finding the highest ready priority takes four
 * word checks no matter how many threads exist. */
#include <arch/context.h>
#include <arch/cpu.h>
#include <arch/paging.h>
#include <arch/percpu.h>
#include <kernel/kprintf.h>
#include <kernel/kstack.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/slab.h>
#include <kernel/string.h>

struct run_queue {
    struct thread *head, *tail;
};

static struct cpu cpu0;
static struct run_queue queues[256];
static uint64_t ready_bitmap[4];       /* bit p set = queues[p] non-empty */
static struct thread *sleepers;        /* unsorted; scanned once per tick */
static struct slab_cache thread_cache;
static uint64_t next_id = 1;

/* ---- run queues ------------------------------------------------------- */

static void enqueue(struct thread *t)
{
    struct run_queue *q = &queues[t->priority];
    t->state = THREAD_READY;
    t->next = NULL;
    if (q->tail)
        q->tail->next = t;
    else
        q->head = t;
    q->tail = t;
    ready_bitmap[t->priority / 64] |= 1ull << (t->priority % 64);
}

static struct thread *dequeue_highest(void)
{
    for (int w = 3; w >= 0; w--) {
        if (!ready_bitmap[w])
            continue;
        int prio = w * 64 + 63 - __builtin_clzll(ready_bitmap[w]);
        struct run_queue *q = &queues[prio];
        struct thread *t = q->head;
        q->head = t->next;
        if (!q->head) {
            q->tail = NULL;
            ready_bitmap[w] &= ~(1ull << (prio % 64));
        }
        t->next = NULL;
        return t;
    }
    return NULL;
}

static int highest_ready_priority(void)
{
    for (int w = 3; w >= 0; w--)
        if (ready_bitmap[w])
            return w * 64 + 63 - __builtin_clzll(ready_bitmap[w]);
    return -1;
}

/* ---- switching -------------------------------------------------------- */

static void reap(struct thread *t)
{
    kstack_free(t->kstack_top);
    if (t->proc) {
        t->proc->run_ticks = t->run_ticks;
        process_thread_gone(t->proc);   /* its address space is inactive now */
    }
    slab_free(&thread_cache, t);
}

/* Runs on the new thread right after every switch (and at the start of
 * every new thread): a dead thread can't free the stack it's running on,
 * so whoever runs next does it. */
static void finish_switch(void)
{
    struct cpu *c = this_cpu();
    if (c->prev && c->prev->state == THREAD_DEAD)
        reap(c->prev);
    c->prev = NULL;
}

/* Pick the next thread and switch to it. The caller has already put the
 * current thread where it belongs (run queue, sleep list, or dead), and
 * interrupts are off. */
static void schedule(void)
{
    struct cpu *c = this_cpu();
    struct thread *prev = c->current;
    struct thread *next = dequeue_highest();
    if (!next)
        next = c->idle;

    c->need_resched = false;
    if (next == prev) {
        prev->state = THREAD_RUNNING;
        return;
    }

    next->state = THREAD_RUNNING;
    next->slice_left = TIMESLICE_TICKS;
    c->current = next;
    c->prev = prev;
    c->switches++;
    arch_set_kernel_stack(next->kstack_top);

    /* User threads run in their process's address space; kernel threads
     * in any (the kernel half is the same everywhere), so we only reload
     * CR3 when a user thread needs a different one. Kernel threads keep
     * whatever is loaded, which saves a TLB flush. A dead process's
     * address space is only freed after we've left it (see reap). */
    uint64_t root = next->proc ? next->proc->root : c->active_root;
    if (prev->state == THREAD_DEAD && prev->proc && root == prev->proc->root)
        root = paging_kernel_root();
    if (root != c->active_root) {
        paging_activate(root);
        c->active_root = root;
    }
    arch_context_switch(&prev->saved_rsp, next->saved_rsp);

    /* We're back: some later schedule() switched to `prev` again. */
    finish_switch();
}

void thread_entry(void (*fn)(void *), void *arg)
{
    finish_switch();
    cpu_sti();              /* new threads start with interrupts on */
    fn(arg);
    thread_exit();
}

/* ---- threads ---------------------------------------------------------- */

static struct thread *thread_alloc(const char *name, void (*fn)(void *), void *arg,
                                   uint8_t priority)
{
    struct thread *t = slab_alloc(&thread_cache);
    if (!t)
        return NULL;
    t->kstack_top = kstack_alloc();
    if (!t->kstack_top) {
        slab_free(&thread_cache, t);
        return NULL;
    }
    size_t n = strlen(name);
    if (n >= sizeof t->name)
        n = sizeof t->name - 1;
    memcpy(t->name, name, n);
    t->priority = priority;
    t->saved_rsp = arch_context_init(t->kstack_top, fn, arg);

    uint64_t flags = irq_save();
    t->id = next_id++;
    irq_restore(flags);
    return t;
}

static struct thread *start_thread(struct thread *t)
{
    uint64_t flags = irq_save();
    enqueue(t);
    struct thread *cur = this_cpu()->current;
    if (t->priority > cur->priority) {
        /* The new thread outranks us: let it run now. */
        this_cpu()->preemptions++;
        enqueue(cur);
        schedule();
    }
    irq_restore(flags);
    return t;
}

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg,
                             uint8_t priority)
{
    struct thread *t = thread_alloc(name, fn, arg, priority);
    return t ? start_thread(t) : NULL;
}

struct thread *thread_create_user(const char *name, void (*fn)(void *), void *arg,
                                  uint8_t priority, struct process *proc)
{
    struct thread *t = thread_alloc(name, fn, arg, priority);
    if (!t)
        return NULL;
    t->proc = proc;
    return start_thread(t);
}

void thread_exit(void)
{
    cpu_cli();
    this_cpu()->current->state = THREAD_DEAD;
    schedule();
    panic("dead thread was scheduled again");
}

void thread_yield(void)
{
    uint64_t flags = irq_save();
    struct cpu *c = this_cpu();
    if (c->current != c->idle)
        enqueue(c->current);
    schedule();
    irq_restore(flags);
}

void thread_sleep(uint64_t ticks)
{
    uint64_t flags = irq_save();
    struct cpu *c = this_cpu();
    struct thread *t = c->current;
    kassert(t != c->idle);
    t->state = THREAD_SLEEPING;
    t->wake_tick = c->ticks + (ticks ? ticks : 1);
    t->next = sleepers;
    sleepers = t;
    schedule();
    irq_restore(flags);
}

struct thread *thread_current(void)
{
    return this_cpu()->current;
}

uint64_t sched_ticks(void)
{
    return this_cpu()->ticks;
}

/* ---- interrupt hooks ---------------------------------------------------- */

/* Interrupts are off here (we're inside the timer handler). */
void sched_timer_tick(void)
{
    struct cpu *c = this_cpu();
    struct thread *cur = c->current;
    c->ticks++;
    cur->run_ticks++;

    /* Wake sleepers whose time has come. */
    struct thread **link = &sleepers;
    while (*link) {
        struct thread *t = *link;
        if (t->wake_tick <= c->ticks) {
            *link = t->next;
            enqueue(t);
            if (t->priority > cur->priority)
                c->need_resched = true;
        } else {
            link = &t->next;
        }
    }

    /* Timeslice: only matters if someone of equal or higher priority is
     * waiting. The idle thread gives way to anything at once. */
    if (cur == c->idle) {
        if (highest_ready_priority() >= 0)
            c->need_resched = true;
    } else if (cur->slice_left > 0 && --cur->slice_left == 0) {
        if (highest_ready_priority() >= cur->priority)
            c->need_resched = true;
        else
            cur->slice_left = TIMESLICE_TICKS;   /* nobody waiting: keep going */
    }
}

void sched_preempt_if_needed(void)
{
    struct cpu *c = this_cpu();
    if (!c->need_resched || !c->current)
        return;
    c->preemptions++;
    if (c->current != c->idle)
        enqueue(c->current);
    schedule();
}

/* ---- startup ---------------------------------------------------------- */

static void idle_loop(void *arg)
{
    (void)arg;
    for (;;)
        cpu_idle();   /* sleep until the next interrupt */
}

void sched_init(void)
{
    arch_percpu_init(&cpu0);
    cpu0.active_root = paging_kernel_root();
    slab_cache_init(&thread_cache, "thread", sizeof(struct thread));
}

void sched_start(void (*fn)(void *), void *arg)
{
    struct thread *idle = thread_alloc("idle", idle_loop, NULL, PRIO_IDLE);
    struct thread *init = thread_alloc("init", fn, arg, PRIO_DEFAULT);
    if (!idle || !init)
        panic("sched: out of memory creating the first threads");

    cpu0.idle = idle;
    cpu0.current = init;
    init->state = THREAD_RUNNING;
    init->slice_left = TIMESLICE_TICKS;
    arch_set_kernel_stack(init->kstack_top);

    kprintf("sched: starting (%u Hz tick, %u-tick timeslice)\n",
            SCHED_HZ, TIMESLICE_TICKS);

    /* Leave the boot stack for good. Its saved RSP goes nowhere. */
    uint64_t boot_rsp;
    arch_context_switch(&boot_rsp, init->saved_rsp);
    panic("returned to the boot stack");
}