/* Scheduler self-tests, run from the init thread (priority 128). */
#include <kernel/kprintf.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/string.h>

/* ---- preemption: two threads that never yield still share the CPU ---- */

struct spinner {
    volatile bool    *stop;
    volatile uint64_t count;
    uint64_t          run_ticks;
    volatile bool     done;   /* one flag per thread: a shared counter would
                                 need an atomic increment, since `x++` can be
                                 preempted between its load and store */
};

static void spin(void *arg)
{
    struct spinner *s = arg;
    while (!*s->stop)
        s->count++;                          /* no yield, no sleep */
    s->run_ticks = thread_current()->run_ticks;
    s->done = true;
}

static void test_preemption(void)
{
    static volatile bool stop;
    static struct spinner a, b;
    stop = false;
    a = (struct spinner){ .stop = &stop };
    b = (struct spinner){ .stop = &stop };

    kassert(thread_create("spin-a", spin, &a, 100));
    kassert(thread_create("spin-b", spin, &b, 100));

    /* While we sleep, only the timer can move the CPU between a and b. */
    thread_sleep(SCHED_HZ / 2);              /* half a second */
    uint64_t ca = a.count, cb = b.count;
    stop = true;
    while (!a.done || !b.done)
        thread_sleep(1);

    kassert(ca > 0 && cb > 0);
    kassert(a.run_ticks >= 5 && b.run_ticks >= 5);
    kprintf("selftest: preemption ok (spin-a %lu ticks, spin-b %lu ticks)\n",
            a.run_ticks, b.run_ticks);
}

/* ---- priority: higher runs first, lower waits until we block ---------- */

static char order[8];
static int order_len;

static void record(void *arg)
{
    order[order_len++] = (char)(uintptr_t)arg;
}

static void test_priority(void)
{
    order_len = 0;
    memset(order, 0, sizeof order);

    kassert(thread_create("high", record, (void *)'H', 200));  /* runs at once */
    record((void *)'I');
    kassert(thread_create("low", record, (void *)'L', 50));    /* has to wait */
    record((void *)'i');
    thread_sleep(2);                                            /* now it runs */

    if (strcmp(order, "HIiL") != 0)
        panic("priority order was \"%s\", expected \"HIiL\"", order);
    kprintf("selftest: priority ok (order %s)\n", order);
}

/* ---- sleep: wakes after at least the requested ticks ---------------- */

static void test_sleep(void)
{
    uint64_t t0 = sched_ticks();
    thread_sleep(10);
    uint64_t elapsed = sched_ticks() - t0;
    kassert(elapsed >= 10 && elapsed <= 12);
    kprintf("selftest: sleep ok (asked 10 ticks, slept %lu)\n", elapsed);
}

/* ---- lifecycle: exited threads give back their stack and struct ----- */

static void nothing(void *arg) { (void)arg; }

static void test_reap(void)
{
    /* One warm-up thread, so page tables for the stack region exist. */
    kassert(thread_create("warmup", nothing, NULL, 200));
    thread_yield();

    uint64_t before = pmm_free_frames();
    for (int i = 0; i < 50; i++)
        kassert(thread_create("short", nothing, NULL, 200));
    thread_yield();                          /* make sure the last one is reaped */
    uint64_t after = pmm_free_frames();
    if (after != before)
        panic("50 thread lifecycles leaked %ld frames", (long)(before - after));
    kprintf("selftest: thread reaping ok (50 threads, no frames leaked)\n");
}

void sched_selftest_boot(void)
{
    test_priority();
    test_sleep();
    test_preemption();
    test_reap();
}

/* ---- destructive: overflow a kernel stack into its guard page -------- */

/* Far more than a 16 KiB stack can hold; volatile so the compiler can't
 * prove the recursion is endless (and refuse to compile it). */
static volatile uint64_t max_depth = 1000000;

static uint64_t recurse(uint64_t depth)
{
    volatile uint8_t pad[256];               /* eat stack quickly */
    pad[0] = (uint8_t)depth;
    if (depth >= max_depth)
        return 0;
    return recurse(depth + 1) + pad[0];      /* not a tail call */
}

static void overflow(void *arg)
{
    (void)arg;
    kprintf("selftest: recursing until the stack overflows\n");
    recurse(0);
}

bool sched_selftest_run(const char *name)
{
    if (strcmp(name, "stack-overflow") == 0) {
        kassert(thread_create("overflow", overflow, NULL, 200));
        panic("selftest '%s' returned; it should have crashed", name);
    }
    return false;
}