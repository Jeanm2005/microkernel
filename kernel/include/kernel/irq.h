/* Hardware interrupts and I/O ports as capabilities.
 *
 * A driver holding an IRQ capability binds it to a notification. Each
 * interrupt on that line then masks the line and signals the
 * notification; the driver handles the device and acknowledges with
 * SYS_IRQ_ACK, which unmasks the line. The kernel itself never touches
 * the device. */
#pragma once
#include <stdint.h>

struct cspace;
struct irq;

/* From the arch code, interrupts off, the line already masked */
void irq_handle(unsigned line);

/* The last capability to `q` is gone (object.c): unbind and mask */
void irq_release(struct irq *q);

/* How many device interrupts were delivered to drivers since boot. */
uint64_t irq_delivered(void);

int64_t sys_irq_bind(struct cspace *cs, int64_t irq, int64_t ntfn, uint64_t bits);
int64_t sys_irq_ack(struct cspace *cs, int64_ irq);
int64_t sys_ioport_enable(struct cspace *cs, int64_t ioport);
