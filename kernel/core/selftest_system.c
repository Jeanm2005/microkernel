/* Starting the system: the kernel's last job at boot.
 *
 * The kernel loads the root task and gives it a factory, every I/O port,
 * the right to claim interrupt lines, and one read-only capability per
 * boot module. Everything after that (which servers run,
 * at what priority, holding which capabilities) is the root task's
 * decision, read from a manifest. For now the demo system runs to
 * completion, so the kernel waits for the root task and checks that it
 * finished cleanly and that every frame came back. */
#include <abi/boot.h>
#include <abi/selftest.h>
#include <abi/syscall.h>
#include <arch/ioport.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/mm.h>
#include <kernel/object.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/process.h>
#include <kernel/selftest.h>
#include <kernel/string.h>

/* What the root task and everything it starts may create in total:
 * kernel objects (endpoints, processes, ...) and pages of RAM for frames. */
#define ROOT_FACTORY_OBJECTS 64
#define ROOT_FACTORY_PAGES   512   /* 2 MiB */

static void give(struct process *p, struct kobj *o, uint32_t rights, int64_t want_slot)
{
    if (!o)
        panic("out of memory setting up the root task");
    int64_t slot = process_give_cap(p, o, rights, 0);
    if (slot != want_slot)
        panic("root task cap landed in slot %ld, not %ld", slot, want_slot);
    kobj_put(o);   /* the root task's capability holds the only reference */
}

/* Run the root task in `mode` until it exits. */
static struct process *run_system(const struct boot_info *boot, uint64_t mode)
{
    const struct boot_module *rm = boot_find_module(boot, "root.elf");
    kassert(rm);
    struct process *root = process_create("root", phys_to_virt(rm->phys), rm->size, mode,
                                          ROOT_TASK_PRIORITY);
    if (!root)
        panic("could not create the root task");

    give(root, factory_create(ROOT_FACTORY_OBJECTS, ROOT_FACTORY_PAGES), CAP_ALL,
         ROOT_SLOT_FACTORY);
    give(root, ioport_create(0, ARCH_IO_PORTS, NULL), CAP_ALL, ROOT_SLOT_IOPORTS);
    give(root, irq_control_create(), CAP_ALL, ROOT_SLOT_IRQ_CONTROL);
    for (size_t i = 0; i < boot->module_count; i++) {
        const struct boot_module *m = &boot->modules[i];
        give(root, module_create(m->name, phys_to_virt(m->phys), m->size), CAP_READ,
             ROOT_SLOT_MODULES + (int64_t)i);
    }

    process_start(root);
    process_wait(root);
    if (root->killed)
        panic("root task was killed: %s", root->kill_reason);
    if (root->exit_code != 0)
        panic("root task exited with code %d", root->exit_code);
    return root;
}

bool system_selftest_wanted(const char *name)
{
    return strcmp(name, "app-crash") == 0 || strcmp(name, "console-input") == 0 ||
           strcmp(name, "driver-restart") == 0;
}

void system_selftest_boot(const struct boot_info *boot)
{
    uint64_t before = pmm_free_frames();
    process_put(run_system(boot, ROOT_MODE_INIT));

    /* The root task, every process it started, their capability tables
     * and every object they made must all be gone. */
    uint64_t after = pmm_free_frames();
    if (after != before)
        panic("the system leaked %ld frames", (long)(before - after));
    kprintf("selftest: system ok (root task ran system.manifest, no frames leaked)\n");
}

bool system_selftest_run(const char *name, const struct boot_info *boot)
{
    if (strcmp(name, "driver-restart") == 0) {
        uint64_t before = pmm_free_frames();
        process_put(run_system(boot, ROOT_MODE_INIT_RESTART));
        kassert(pmm_free_frames() == before);
        kprintf("selftest: disk driver restarted; filesystem survived; no frames leaked\n");
        return true;
    }
    if (strcmp(name, "app-crash") == 0) {
        process_put(run_system(boot, ROOT_MODE_INIT_CRASH));
        kprintf("selftest: an application crashed; the root task and the console carried on\n");
        return true;
    }
    if (strcmp(name, "console-input") == 0) {
        uint64_t before = irq_delivered();
        process_put(run_system(boot, ROOT_MODE_INIT_INPUT));
        if (irq_delivered() == before)
            panic("the console finished without a single serial interrupt");
        kprintf("selftest: the console received input through IRQ 4 (%lu interrupts)\n",
                irq_delivered() - before);
        return true;
    }
    return false;
}