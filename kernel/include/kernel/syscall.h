#pragma once
#include <stdint.h>

/* Portable syscall handler: the architecture code decodes registers and
 * calls this. Returns the value for the user's result register. */
uint64_t syscall_handle(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5);