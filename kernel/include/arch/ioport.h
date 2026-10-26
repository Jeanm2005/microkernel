/* I/O ports (x86's separate 16-bit address space for device registers,
 * reached with the in/out instructions). User code may only touch the
 * ports its process has been allowed. */
#pragma once
#include <stdint.h>

#define ARCH_IO_PORTS 65536

/* Load the running process's permissions: one bit per port, 1 = allowed,
 * of which only bytes [lo, hi) can have any bit set; or NULL for "no
 * ports" (kernel threads and most processes). Call with interrupts off. */
void arch_set_io_bitmap(const uint8_t *allowed, uint32_t lo, uint32_t hi);