#include <stdbool.h>
#include <arch/backtrace.h>
#include <arch/cpu.h>
#include <kernel/kprintf.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include "lapic.h"
#include "trap.h"

enum {
    VEC_BREAKPOINT = 3,
    VEC_DOUBLE_FAULT = 8,
    VEC_PAGE_FAULT = 14,
};

static const char *const exception_names[32] = {
    [0]  = "DIVIDE ERROR (#DE)",
    [1]  = "DEBUG (#DB)",
    [2]  = "NON-MASKABLE INTERRUPT (NMI)",
    [3]  = "BREAKPOINT (#BP)",
    [4]  = "OVERFLOW (#OF)",
    [5]  = "BOUND RANGE EXCEEDED (#BR)",
    [6]  = "INVALID OPCODE (#UD)",
    [7]  = "DEVICE NOT AVAILABLE (#NM)",
    [8]  = "DOUBLE FAULT (#DF)",
    [9]  = "COPROCESSOR SEGMENT OVERRUN",
    [10] = "INVALID TSS (#TS)",
    [11] = "SEGMENT NOT PRESENT (#NP)",
    [12] = "STACK-SEGMENT FAULT (#SS)",
    [13] = "GENERAL PROTECTION FAULT (#GP)",
    [14] = "PAGE FAULT (#PF)",
    [16] = "x87 FLOATING-POINT ERROR (#MF)",
    [17] = "ALIGNMENT CHECK (#AC)",
    [18] = "MACHINE CHECK (#MC)",
    [19] = "SIMD FLOATING-POINT ERROR (#XM)",
    [20] = "VIRTUALIZATION EXCEPTION (#VE)",
    [21] = "CONTROL PROTECTION (#CP)",
    [28] = "HYPERVISOR INJECTION (#HV)",
    [29] = "VMM COMMUNICATION (#VC)",
    [30] = "SECURITY EXCEPTION (#SX)",
};

static bool from_user(const struct trap_frame *tf)
{
    return (tf->cs & 3) == 3;
}

static void dump_registers(const struct trap_frame *tf)
{
    kprintf("  rip %016lx  cs %04lx  rflags %016lx\n", tf->rip, tf->cs, tf->rflags);
    kprintf("  rsp %016lx  ss %04lx  error  %016lx\n", tf->rsp, tf->ss, tf->error_code);
    kprintf("  rax %016lx  rbx %016lx  rcx %016lx\n", tf->rax, tf->rbx, tf->rcx);
    kprintf("  rdx %016lx  rsi %016lx  rdi %016lx\n", tf->rdx, tf->rsi, tf->rdi);
    kprintf("  rbp %016lx  r8  %016lx  r9  %016lx\n", tf->rbp, tf->r8, tf->r9);
    kprintf("  r10 %016lx  r11 %016lx  r12 %016lx\n", tf->r10, tf->r11, tf->r12);
    kprintf("  r13 %016lx  r14 %016lx  r15 %016lx\n", tf->r13, tf->r14, tf->r15);
    kprintf("  cr0 %016lx  cr2 %016lx  cr3 %016lx  cr4 %016lx\n",
            read_cr0(), read_cr2(), read_cr3(), read_cr4());
}

/* Page-fault error code bits, set by the CPU. */
#define PF_PRESENT (1u << 0)   /* 1 = protection violation, 0 = page not mapped */
#define PF_WRITE   (1u << 1)
#define PF_USER    (1u << 2)
#define PF_RSVD    (1u << 3)   /* a reserved bit was set in a page-table entry */
#define PF_IFETCH  (1u << 4)   /* instruction fetch (e.g. NX violation) */

static void explain_page_fault(const struct trap_frame *tf)
{
    uint64_t e = tf->error_code;
    kprintf("  cause: %s %s page at %p (%s mode)%s\n",
            (e & PF_IFETCH) ? "instruction fetch from" :
            (e & PF_WRITE)  ? "write to" : "read from",
            (e & PF_PRESENT) ? "a protected" : "a non-present",
            (void *)read_cr2(),
            (e & PF_USER) ? "user" : "kernel",
            (e & PF_RSVD) ? " [reserved bit set in page table]" : "");
}

__attribute__((noreturn))
static void fatal(const struct trap_frame *tf, const char *what)
{
    kprintf("\n*** %s at rip %p (%s mode)\n", what, (void *)tf->rip,
            from_user(tf) ? "user" : "kernel");
    if (tf->vector == VEC_PAGE_FAULT)
        explain_page_fault(tf);
    dump_registers(tf);

    if (from_user(tf)) {
        /* A user program broke, not the kernel: report it, end that one
         * process, and carry on. (No backtrace: user frame pointers are
         * untrusted, and the kernel shouldn't chase them.) In M9 the
         * process's supervisor gets told instead, so it can restart it. */
        process_kill_current(what);
    }

    kprintf("  backtrace:\n");
    kprintf("    %p\n", (void *)tf->rip);
    backtrace_print(tf->rbp);
    panic("unhandled %s", what);
}

void trap_dispatch(struct trap_frame *tf)
{
    uint64_t v = tf->vector;

    if (v == VEC_BREAKPOINT) {
        /* #BP is a "trap": rip already points past the int3, so returning
         * resumes execution. Debuggers rely on this. */
        kprintf("trap: breakpoint at rip %p, resuming\n", (void *)(tf->rip - 1));
    } else if (v < 32) {
        const char *name = exception_names[v];
        fatal(tf, name ? name : "RESERVED EXCEPTION");
    } else if (v == VEC_TIMER) {
        /* Acknowledge first: if the scheduler switches threads below, this
         * handler doesn't finish until we switch back, and the LAPIC would
         * hold back further ticks until then. */
        lapic_eoi();
        sched_timer_tick();
    } else if (v == VEC_LAPIC_SPURIOUS || (v >= VEC_PIC_BASE && v < VEC_PIC_BASE + 16)) {
        /* Spurious interrupts need no EOI and no handling. */
    } else {
        fatal(tf, "UNEXPECTED INTERRUPT");
    }

    /* The single place where an interrupted thread can lose the CPU. Its
     * trap frame stays on its own kernel stack; when it is scheduled again
     * it returns from here and `iretq`s back to what it was doing. */
    sched_preempt_if_needed();
}