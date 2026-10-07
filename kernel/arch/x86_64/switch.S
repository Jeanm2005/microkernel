/* Kernel thread switching. See include/arch/context.h. */

    .text

/* void arch_context_switch(uint64_t *save_rsp [rdi], uint64_t load_rsp [rsi])
 *
 * Only the callee-saved registers (rbx, rbp, r12-r15) need saving: the C
 * compiler already assumes every other register is clobbered by a call.
 * RFLAGS isn't saved; the scheduler always switches with interrupts off,
 * and each thread restores its own interrupt state when it resumes. */
    .global arch_context_switch
arch_context_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

/* A new thread's first "return" lands here (arch_context_init put this
 * address where arch_context_switch's `ret` will find it). r12 = fn,
 * r13 = arg. RBP = 0 ends the backtrace chain. */
    .global thread_trampoline
thread_trampoline:
    movq %r12, %rdi
    movq %r13, %rsi
    xorl %ebp, %ebp
    call thread_entry
    ud2                     /* thread_entry never returns */

    .section .note.GNU-stack, "", @progbits