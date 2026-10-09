#include <arch/cpu.h>
#include <arch/user.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include "gdt.h"
#include "trap.h"

#define MSR_STAR 0xc0000081 /* segment selectors for syscall/sysret */
#define MSR_LSTAR 0xc0000082 /* syscall entry point */
#define MSR_SFMASK 0xc0000084 /* RFLAGS bits cleared on syscall */
#define EFER_SCE (1ull << 0) /* syscall enable */

#define RFLAGS_TF   (1ull << 8)
#define RFLAGS_IF   (1ull << 9)
#define RFLAGS_DF   (1ull << 10)
#define RFLAGS_AC   (1ull << 18)

extern char syscall_entry[];

void arch_syscall_init(void)
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    /* STAR[47:32]: syscall loads CS = this, SS = this + 8.
     * STAR[63:48]: sysret loads SS = this + 8, CS = this + 16 (RPL 3).
     * That second rule is why the GDT has user data right before user code. */
    wrmsr(MSR_STAR, ((uint64_t)(GDT_KERNEL_DATA | 3) << 48) |
                    ((uint64_t)GDT_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    /* Enter with interrupts off (until we're on the kernel stack), the
     * direction flag clear (the C ABI requires it), and no single-step or
     * alignment-check traps inherited from user mode. */
    wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_DF | RFLAGS_TF | RFLAGS_AC);
}

/* Called from syscall_entry with interrupts on. */
void syscall_dispatch(struct trap_frame *tf)
{
    tf->rax = syscall_handle(tf->rax, tf->rdi, tf->rsi, tf->rdx,
                             tf->r10, tf->r8, tf->r9);
    cpu_cli();
    sched_preempt_if_needed();
}