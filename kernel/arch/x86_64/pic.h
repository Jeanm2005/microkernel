/* The 8259 PIC (programmable interrupt controller): the original PC
 * interrupt controller, still present on every PC chipset and wired to the
 * LAPIC's LINT0 input. Its 16 lines carry the legacy ISA devices' interrupts
 * (IRQ 4 = COM1). Vectors VEC_PIC_BASE..+15. */
#pragma once
#include <stdbool.h>

/* Remap both PICs to VEC_PIC_BASE and mask every line. */
void pic_init(void);

/* An interrupt arrived on vector VEC_PIC_BASE + line. Returns false if it
 * was spurious (no EOI then). Otherwise masks the line, sends the EOI and
 * returns true; the caller passes it on. */
bool pic_accept(unsigned line);
