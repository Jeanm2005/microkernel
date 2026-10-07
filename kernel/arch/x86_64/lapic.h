/* LAPIC (local Advanced Programmable Interrupt Controller): the per-CPU
 * interrupt controller. It owns the timer we schedule with, and every
 * interrupt it delivers must be acknowledged with an EOI (end of
 * interrupt) or it won't deliver another of the same or lower priority. */
#pragma once

/* Interrupt vectors. 0x20-0x2f are where the legacy PIC is parked
 * (masked) so its stray "spurious" interrupts can't look like CPU
 * exceptions (vectors 0-31). */
#define VEC_PIC_BASE      0x20
#define VEC_TIMER         0x30
#define VEC_LAPIC_SPURIOUS 0xff

void lapic_eoi(void);