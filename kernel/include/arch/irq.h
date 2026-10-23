/* Hardware interrupt lines, as the portable kernel sees them. The arch
 * code calls irq_handle() (kernel/irq.h) for each one that fires, with the
 * line already masked; it stays masked until someone unmasks it. */
#pragma once

#define ARCH_IRQ_LINES 16 /* the legacy ISA lines 0-15 */

void arch_irq_mask(unsigned line);
void arch_irq_unmask(unsigned line);
