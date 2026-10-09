/* User-mode self-tests: run the root task and check how it ended. */
#include <abi/selftest.h>
#include <kernel/kprintf.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/string.h>

#define ROOT_PRIORITY 100   /* below init (128), which just waits for it */

static const struct boot_module *root_module(const struct boot_info *boot)
{
    const struct boot_module *m = boot_find_module(boot, "root.elf");
    if (!m)
        panic("root task module 'root.elf' was not loaded by the bootloader");
    return m;
}

/* Run the root task in `mode` to completion. Returns the finished process;
 * the caller must process_put() it. */
static struct process *run_root(const struct boot_info *boot, uint64_t mode)
{
    const struct boot_module *m = root_module(boot);
    struct process *p = process_spawn("root", phys_to_virt(m->phys), m->size,
                                      mode, ROOT_PRIORITY);
    if (!p)
        panic("could not start the root task");
    process_wait(p);
    return p;
}

/* Competes with the root task for the CPU at the same priority. */
struct competitor {
    volatile bool stop;
    volatile bool done;
    uint64_t run_ticks;
};

static void compete(void *arg)
{
    struct competitor *c = arg;
    while (!c->stop)
        ;
    c->run_ticks = thread_current()->run_ticks;
    c->done = true;
}

void user_selftest_boot(const struct boot_info *boot)
{
    static struct competitor comp;
    comp = (struct competitor){ 0 };
    uint64_t before = pmm_free_frames();

    /* The root task spends ~10^9 cycles computing without system calls.
     * If both it and this kernel thread get real CPU time, the timer has
     * preempted ring-3 code and the kernel has resumed it correctly. */
    kassert(thread_create("competitor", compete, &comp, ROOT_PRIORITY));
    struct process *p = run_root(boot, ROOT_MODE_NORMAL);
    comp.stop = true;
    while (!comp.done)
        thread_sleep(1);
    thread_sleep(1);   /* let the competitor be reaped before counting frames */

    if (p->killed)
        panic("root task was killed: %s", p->kill_reason);
    if (p->exit_code != 0)
        panic("root task exited with code %d", p->exit_code);
    if (p->run_ticks < 5 || comp.run_ticks < 5)
        panic("root task (%lu ticks) and competitor (%lu ticks) didn't share the CPU",
              p->run_ticks, comp.run_ticks);
    uint64_t root_ticks = p->run_ticks;
    process_put(p);

    /* Its page tables, code, data and stack must all have come back. */
    uint64_t after = pmm_free_frames();
    if (after != before)
        panic("root task leaked %ld frames", (long)(before - after));
    kprintf("selftest: user mode ok (root task exited with code 0, no frames leaked)\n");
    kprintf("selftest: user preemption ok (root task %lu ticks, kernel competitor %lu ticks)\n",
            root_ticks, comp.run_ticks);
}

bool user_selftest_run(const char *name, const struct boot_info *boot)
{
    uint64_t mode;
    if (strcmp(name, "user-pagefault") == 0)
        mode = ROOT_MODE_PAGEFAULT;
    else if (strcmp(name, "user-privileged") == 0)
        mode = ROOT_MODE_PRIVILEGED;
    else if (strcmp(name, "user-kernel-read") == 0)
        mode = ROOT_MODE_KERNEL_READ;
    else
        return false;

    /* Unlike the other destructive tests, the kernel must survive this
     * one: only the misbehaving process dies. */
    struct process *p = run_root(boot, mode);
    if (!p->killed)
        panic("root task in mode %lu was not killed", mode);
    kprintf("selftest: kernel survived a faulting user process (%s)\n", p->kill_reason);
    process_put(p);
    return true;
}