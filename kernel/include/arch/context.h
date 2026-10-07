/* Switching the CPU between kernel threads. */
#pragma once
#include <stdint.h>

/* Save the callee-saved registers on the current stack, store the stack
 * pointer in *save_rsp, load `load_rsp` and restore the registers saved
 * there. Returns when some later switch comes back to *save_rsp. */
 void arch_context_switch(uint64_t *save_rsp, uint64_t load_rsp);

 /* Prepare a fresh stack so the first arch_context_switch() to it calls
 * thread_entry(fn, arg). Returns the stack pointer to switch to. */
 uint64_t arch_context_init(uint64_t stack_top, void (*fn)(void *), void *arg);

 /* Kernel stack the CPU loads when this thread traps from user mode. */
 void arch_set_kernel_stack(uint64_t stack_top);