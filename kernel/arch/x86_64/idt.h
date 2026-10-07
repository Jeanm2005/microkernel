/* IDT (Interrupt Descriptor Table): one entry ("gate") per vector 0-255
 * telling the CPU which code to run for that exception or interrupt.
 * Vectors 0-31 are CPU exceptions; 32-255 are free for device interrupts
 * and IPIs (inter-processor interrupts). */
#pragma once

void idt_init(void);