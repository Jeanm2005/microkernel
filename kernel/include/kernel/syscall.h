#pragma once
#include <stdint.h>

/* A system call's register state in portable form. The architecture code
 * fills it from the trap frame, calls syscall_handle(), and copies arg[]
 * back, because IPC calls return results in the argument registers too
 * (see include/abi/syscall.h). */
struct syscall_regs {
    uint64_t nr;
    uint64_t arg[6];
};

/* Returns the value for the user's result register (rax). */
uint64_t syscall_handle(struct syscall_regs *r);