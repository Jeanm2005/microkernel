/* I/O ports *x86's separate 16-bit address space for device registers,
 * reached with the in/out instructions). User code may only touch the 
 * ports its process has been allowed. */
#pragma once
#include <stdint.h>

/* Ports 0..ARCH_IO_PORTS-1 can be granted to user space; higher ones are
 * always refused. 1024 covers every legacy PC device (COM ports at
 * 0x3f8/0x2f8, keyboard controller, PIT, CMOS clock, ...) */
#define ARCH_IO_PORTS 1024

/* Load the running process's permissions: one bit per port, 1 = allowed,
 * or NULL for "no ports" (kernel threads and most processes). Call with
 * interrupts off. */
void arch_set_io_bitmap(const uint8_t *allowed);
