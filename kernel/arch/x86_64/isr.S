/* Entry stubs for all 256 interrupt vectors, plus the common path that
 * saves registers and calls trap_dispatch() in C.
 *
 * Some exceptions push an error code and others don't. Each stub pushes a
 * dummy 0 when the CPU didn't, then the vector number, so every trap ends
 * up with the same stack layout (struct trap_frame in trap.h). */

    .set vec, 0

    .section .rodata
    .balign 8
    .global isr_stub_table
isr_stub_table:

    .rept 256
    .section .text
    .balign 16
1:
    /* Vectors where the CPU pushes an error code itself:
     * 8 #DF, 10 #TS, 11 #NP, 12 #SS, 13 #GP, 14 #PF, 17 #AC, 21 #CP, 29 #VC, 30 #SX */
    .if !(vec == 8 || vec == 10 || vec == 11 || vec == 12 || vec == 13 || vec == 14 || vec == 17 || vec == 21 || vec == 29 || vec == 30)
    pushq $0
    .endif
    pushq $vec
    jmp isr_common

    .section .rodata
    .quad 1b
    .set vec, vec + 1
    .endr

    .section .text
isr_common:
    /* From ring 3, GS base is still the user's: swap in the kernel's.
     * At this point 24(%rsp) is the CS the CPU pushed (above the vector,
     * the error code and RIP); its low 2 bits are the old privilege level. */
    testb $3, 24(%rsp)
    jz 1f
    swapgs
1:
    cld
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

    /* The CPU aligned RSP to 16 bytes before pushing its 5-word frame;
     * 5 + 2 (error code, vector) + 15 registers = 22 words = 176 bytes,
     * so RSP is still 16-byte aligned here, as the C ABI requires. */
    movq %rsp, %rdi
    call trap_dispatch

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
    addq $16, %rsp          /* drop vector and error code */
    testb $3, 8(%rsp)       /* returning to ring 3? (CS is now at 8(%rsp)) */
    jz 2f
    swapgs
2:
    iretq

    /* Tell the linker this file doesn't need an executable stack. */
    .section .note.GNU-stack, "", @progbits