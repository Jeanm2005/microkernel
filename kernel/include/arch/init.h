#pragma once

/* Set up this CPU's descriptor tables: GDT (Global Descriptor Table),
 * TSS (Task State Segment) and IDT (Interrupt Descriptor Table).
 * After this returns, CPU exceptions are caught and reported instead of
 * triple-faulting the machine. Also enables the `syscall` instruction.
 * Interrupts stay disabled. */
void arch_init_cpu(void);