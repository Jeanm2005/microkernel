/* Entry and exit paths between ring 3 and the kernel. */

/* Offsets into struct cpu (include/kernel/percpu.h). */
#define CPU_USER_RSP 8
#define CPU_KERNEL_RSP 16
#define USER_CS 0x23 /* GDT_USER_CODE | RPL 3 */
#define USER_SS 0x1b /* GDT_USER_DATA | RPL 3 */
#define SYSCALL_VECTOR 0x100 /* marks syscall frames in trap dumps */

    .text

/* `syscall` lands here with: rcx = user rip, r11 = user rflags, interrupts
 * off (SFMASK), still on the USER stack and with the USER GS base. So:
 *  1. swapgs to reach per-CPU data,
 *  2. switch to this thread's kernel stack,
 *  3. build a struct trap_frame identical to an interrupt's, so the
 *     return path, crash dumps and the scheduler treat both alike. */
    .global syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:CPU_USER_RSP
    movq %gs:CPU_KERNEL_RSP, %rsp

    pushq $USER_SS
    pushq %gs:CPU_USER_RSP
    pushq %r11                  /* rflags */
    pushq $USER_CS
    pushq %rcx                  /* rip */
    pushq $0                    /* error code */
    pushq $SYSCALL_VECTOR

    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rdi
    sti                         /* the kernel is preemptible during syscalls */
    call syscall_dispatch       /* returns with interrupts off */

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp

    /* Return with iretq rather than sysretq. It's a little slower, but the
     * frame may have been changed (e.g. a process killed and another
     * thread resumed), and sysretq with a non-canonical user rip faults in
     * ring 0 on Intel CPUs, a classic kernel security bug. */
    swapgs
    iretq

/* void arch_enter_user(uint64_t rip [rdi], uint64_t rsp [rsi], uint64_t arg [rdx]) */
    .global arch_enter_user
arch_enter_user:
    cli
    pushq $USER_SS
    pushq %rsi
    pushq $0x202                /* rflags: IF (interrupts on) + reserved bit 1 */
    pushq $USER_CS
    pushq %rdi

    movq %rdx, %rdi             /* main(arg) */
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %ebp, %ebp
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d

    swapgs                      /* kernel GS base -> user (0) */
    iretq

    .section .note.GNU-stack, "", @progbits