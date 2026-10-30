/* Hardware interrupts and I/O ports as capabilities.
 *
 * Interrupt lines are claimed from the IRQ control capability, once each:
 * whoever holds the resulting IRQ capability owns the line until the last
 * copy is gone. A driver binds it to a notification. Each interrupt on
 * that line then masks the line and signals the notification; the driver
 * handles the device and acknowledges with SYS_IRQ_ACK, which unmasks the
 * line. The kernel itself never touches the device. */
#pragma once
#include <stdint.h>

struct cspace;
struct irq;
struct process;

/* From the arch code, interrupts off, the line already masked. */
void irq_handle(unsigned line);

/* The last capability to `q` is gone (object.c): unbind, mask, release. */
void irq_release(struct irq *q);

/* How many device interrupts were delivered to drivers since boot. */
uint64_t irq_delivered(void);

int64_t sys_irq_get(struct cspace *cs, int64_t factory, int64_t control, uint64_t line);
int64_t sys_irq_bind(struct cspace *cs, int64_t irq, int64_t ntfn, uint64_t bits);
int64_t sys_irq_ack(struct cspace *cs, int64_t irq);

int64_t sys_ioport_enable(struct cspace *cs, int64_t ioport);
int64_t sys_ioport_sub(struct cspace *cs, int64_t factory, int64_t ioport, uint64_t base,
                       uint64_t count);
/* The process is going away: free its I/O permission bitmap. */
void ioport_release(struct process *p);
int64_t sys_irq_unbind(struct cspace *cs, int64_t irq);
