/* x86 port I/O for drivers. These only work for ports the process enabled
 * with SYS_IOPORT_ENABLE; any other port makes the CPU raise a general
 * protection fault, and the kernel kills the process. */
#pragma once
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val)
{
	__asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t pport)
{
	uint8_t v;
	__asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory");
	return v;
}
