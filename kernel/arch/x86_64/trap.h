#pragma once
#include <stdint.h>

/* Register state at the moment of a trap, as laid out on the stack by
 * isr.S (lowest address first). The last five fields are pushed by the
 * CPU itself. */
struct trap_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;   /* 0 for vectors that don't push one */
    uint64_t rip, cs, rflags, rsp, ss;
};

void trap_dispatch(struct trap_frame *tf);