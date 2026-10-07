/* Scheduler tick: LAPIC timer in periodic mode, calibrated against the
 * PIT (programmable interval timer, the old fixed-frequency 1.193182 MHz
 * timer chip), whose rate is known exactly. */
#include <arch/cpu.h>
#include <arch/paging.h>
#include <arch/timer.h>
#include <kernel/kprintf.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include "lapic.h"

#define MSR_APIC_BASE      0x1b
#define APIC_BASE_ENABLE   (1ull << 11)
#define APIC_BASE_X2APIC   (1ull << 10)

/* LAPIC register offsets */
#define LAPIC_TPR          0x080   /* task priority: 0 = accept everything */
#define LAPIC_EOI          0x0b0
#define LAPIC_SVR          0x0f0   /* spurious vector + software enable */
#define LAPIC_LVT_TIMER    0x320
#define LAPIC_TIMER_INIT   0x380
#define LAPIC_TIMER_CUR    0x390
#define LAPIC_TIMER_DIV    0x3e0

#define SVR_ENABLE         (1u << 8)
#define LVT_MASKED         (1u << 16)
#define LVT_PERIODIC       (1u << 17)
#define DIV_BY_16          0x3

#define PIT_HZ             1193182u
#define CALIBRATE_MS       10

static volatile uint32_t *lapic;

static uint32_t lapic_read(uint32_t reg) { return lapic[reg / 4]; }
static void lapic_write(uint32_t reg, uint32_t v) { lapic[reg / 4] = v; }

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

/* The 8259 PIC (programmable interrupt controller) is the pre-APIC
 * interrupt controller. We don't use it, but it still exists: move its
 * vectors away from the CPU exception range, then mask every line. */
static void pic_disable(void)
{
    outb(0x20, 0x11); outb(0xa0, 0x11);                  /* start init sequence */
    outb(0x21, VEC_PIC_BASE); outb(0xa1, VEC_PIC_BASE + 8);
    outb(0x21, 0x04); outb(0xa1, 0x02);                  /* cascade wiring */
    outb(0x21, 0x01); outb(0xa1, 0x01);                  /* 8086 mode */
    outb(0x21, 0xff); outb(0xa1, 0xff);                  /* mask all */
}

static void lapic_enable(void)
{
    uint64_t base = rdmsr(MSR_APIC_BASE);
    if (base & APIC_BASE_X2APIC)
        panic("timer: x2APIC mode is on; only xAPIC (MMIO) is supported");
    wrmsr(MSR_APIC_BASE, base | APIC_BASE_ENABLE);

    /* The LAPIC's registers are MMIO (memory-mapped I/O): map them,
     * uncached, at the usual HHDM address for their physical page. */
    uint64_t phys = base & 0x000ffffffffff000ull;
    if (!paging_map(paging_kernel_root(), (uint64_t)phys_to_virt(phys), phys,
                    PAGE_WRITE | PAGE_UNCACHED))
        panic("timer: could not map the LAPIC at %p", (void *)phys);
    lapic = phys_to_virt(phys);

    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SVR, SVR_ENABLE | VEC_LAPIC_SPURIOUS);
}

/* Count LAPIC timer ticks (divide-by-16) during CALIBRATE_MS, timed by PIT
 * channel 2. Channel 2's output is readable at port 0x61 bit 5, and its
 * gate (start/stop) is port 0x61 bit 0, so we can poll it without
 * interrupts. */
static uint32_t calibrate(void)
{
    uint16_t count = PIT_HZ / (1000 / CALIBRATE_MS);

    uint8_t p61 = inb(0x61) & ~0x03;       /* gate low, speaker off */
    outb(0x61, p61);
    outb(0x43, 0xb0);                      /* channel 2, lo/hi byte, mode 0 */
    outb(0x42, count & 0xff);
    outb(0x42, count >> 8);

    lapic_write(LAPIC_TIMER_DIV, DIV_BY_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);   /* one-shot, no interrupt */
    lapic_write(LAPIC_TIMER_INIT, 0xffffffff);

    outb(0x61, p61 | 0x01);                /* gate high: PIT starts counting */
    while (!(inb(0x61) & 0x20))            /* output goes high at zero */
        cpu_relax();

    uint32_t elapsed = 0xffffffff - lapic_read(LAPIC_TIMER_CUR);
    lapic_write(LAPIC_TIMER_INIT, 0);      /* stop */
    outb(0x61, p61);
    return elapsed;
}

void arch_timer_init(uint32_t hz)
{
    pic_disable();
    lapic_enable();

    uint32_t per_calib = calibrate();
    if (per_calib == 0)
        panic("timer: LAPIC timer did not count during calibration");
    uint64_t per_second = (uint64_t)per_calib * (1000 / CALIBRATE_MS);
    uint32_t initial = (uint32_t)(per_second / hz);

    lapic_write(LAPIC_TIMER_DIV, DIV_BY_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | VEC_TIMER);
    lapic_write(LAPIC_TIMER_INIT, initial);

    kprintf("timer: LAPIC at %p, %u ticks per %d ms, periodic at %u Hz\n",
            (void *)lapic, per_calib, CALIBRATE_MS, hz);
}