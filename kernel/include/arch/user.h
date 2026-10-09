/* Entering and leaving user mode (ring 3). */
#pragma once
#include <stdint.h>

/* Lowest and highest-plus-one user virtual addresses. Page 0 is never
 * mapped, so null-pointer use always faults. */
#define USER_BASE 0x1000ull
#define USER_TOP  0x0000800000000000ull

/* Enable the `syscall` instruction (called once per CPU at boot). */
void arch_syscall_init(void);

/* Drop to ring 3 at `rip` with stack `rsp` and `arg` in the first
 * argument register. All other registers are zeroed so no kernel values
 * leak. The current address space must already be the process's. */
__attribute__((noreturn))
void arch_enter_user(uint64_t rip, uint64_t rsp, uint64_t arg);