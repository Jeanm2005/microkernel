/* Program entry. The kernel starts us with RSP 16-byte aligned at the top
 * of the user stack and the startup argument in rdi, which is exactly
 * main's first argument. Whatever main returns becomes the exit code. */
    .text
    .global _start
_start:
    xorl %ebp, %ebp          /* end of the frame-pointer chain */
    call main
    movl %eax, %edi
    movl $1, %eax            /* SYS_EXIT */
    syscall
    ud2                      /* SYS_EXIT never returns */

    .section .note.GNU-stack, "", @progbits