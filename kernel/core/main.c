#include <stdint.h>
#include <arch/cpu.h>
#include <arch/init.h>
#include <arch/paging.h>
#include <arch/qemu.h>
#include <arch/selftest.h>
#include <arch/serial.h>
#include <arch/timer.h>
#include <kernel/boot.h>
#include <kernel/cmdline.h>
#include <kernel/kprintf.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>

static struct boot_info boot;

const char *mem_type_name(enum mem_type t)
{
    static const char *names[] = {
        [MEM_USABLE] = "usable",
        [MEM_RESERVED] = "reserved",
        [MEM_ACPI_RECLAIMABLE] = "acpi-reclaim",
        [MEM_ACPI_NVS] = "acpi-nvs",
        [MEM_BAD] = "bad",
        [MEM_BOOTLOADER_RECLAIMABLE] = "boot-reclaim",
        [MEM_KERNEL_AND_MODULES] = "kernel+mods",
        [MEM_FRAMEBUFFER] = "framebuffer",
    };
    return t < sizeof names / sizeof names[0] ? names[t] : "?";
}

static void print_memory_map(void)
{
    uint64_t usable = 0;
    kprintf("memory map (%lu regions):\n", (unsigned long)boot.region_count);
    for (size_t i = 0; i < boot.region_count; i++) {
        struct mem_region *r = &boot.regions[i];
        kprintf("  %016lx-%016lx %-12s %8lu KiB\n",
                r->base, r->base + r->length,
                mem_type_name(r->type), r->length / 1024);
        if (r->type == MEM_USABLE)
            usable += r->length;
    }
    kprintf("usable RAM: %lu MiB\n", usable >> 20);
}

/* First thread. Runs on its own stack, so the bootloader's memory can now
 * be reclaimed, then runs the scheduler tests and any requested selftest. */
static void kinit(void *arg)
{
    (void)arg;
    kprintf("sched: running on thread '%s' (id %lu)\n",
            thread_current()->name, thread_current()->id);
    pmm_reclaim_bootloader(&boot);

    sched_selftest_boot();

    char test[32];
    if (cmdline_get(boot.cmdline, "selftest", test, sizeof test)) {
        kprintf("selftest: running '%s'\n", test);
        if (!core_selftest_run(test) && !sched_selftest_run(test) &&
            !arch_selftest_run(test))
            panic("unknown selftest '%s'", test);
    }

    kprintf("kernel: init complete\n");
    qemu_exit(0);
}

/* Entry point. Limine has put us in 64-bit long mode with paging on, the
 * kernel mapped at 0xffffffff80000000, all RAM mapped at hhdm_offset,
 * interrupts off and a 64 KiB stack. */
void kmain(void)
{
    serial_init();
    kprintf("\nmicrokernel: booting (x86_64)\n");

    /* First, so that any fault from here on is reported, not a reboot. */
    arch_init_cpu();
    kprintf("cpu: GDT, TSS and IDT loaded\n");

    boot_info_collect(&boot);
    kprintf("kernel: phys %p virt %p, hhdm offset %p\n",
            (void *)boot.kernel_phys_base, (void *)boot.kernel_virt_base,
            (void *)boot.hhdm_offset);
    kprintf("cmdline: \"%s\"\n", boot.cmdline);
    print_memory_map();

    pmm_init(&boot);
    paging_init(&boot);
    sched_init();

    arch_selftest_boot();
    core_selftest_boot();

    arch_timer_init(SCHED_HZ);
    sched_start(kinit, NULL);   /* does not return */
}