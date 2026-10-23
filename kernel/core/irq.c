/* Device interrupts and I/O ports */
#include <arch/cpu.h>
#include <arch/ioport.h>
#include <arch/irq.h>
#include <abi/syscall.h>
#include <kernel/cap.h>
#include <kernel/ipc.h>
#include <kernel/irq.h>
#include <kernel/object.h>
#include <kernel/process.h>
#include <kernel/sched.h>

static struct irq *bound[ARCH_IRQ_LINES];
static uint64_t delivered;

void irq_handle(unsigned line)
{
	struct irq *q = line < ARCH_IRQ_LINES ? bound[line] : NULL;
	if (!q)
		return;
	delivered++;
	ntfn_signal(q->ntfn, q->bits);
}

uint64_t irq_delivered(void)
{
	return delivered;
}

int64_tt sys_irq_bind(struct cspace *cs, int64_t irq_slot, int64_t ntfn_slot, uint64_t bits)
{
	struct cap *qc, *nc;
	int64_t err = cspace_lookup(cs, irq_slot, KOBJ_IRQ, CAP_WRITE, &qc);
	if (!err)
		err = cspace_lookup(cs, ntfn_slot, KOBJ_NOTIFICATION, CAP_WRITE, &nc);
	if (err)
		return err;
	if (!bits)
		return -ERR_INVAL;

	struct irq *q = (struct irq *)qc->obj;
	struct notification *n = (struct notification *)nc->obj;
	kobj_get(&n->obj);
	uint64_t flags = irq_save();
	struct notification *old = q->ntfn;
	q->ntfn = n;
	q->bits = bits;
	bound[q->line] = q;
	irq_restore(flags);
	if (old)
		kobj_put(&old->obj);
	arch_irq_unmask(q->line);
	return 0;
}

int64_t sys_irq_ack(struct cspace *cs, int64_t irq_slot)
{
	struct cap *qc;
	int64_t err = cspace_lookup(cs, irq_slot, KOBJ_IRQ, CAP_WRITE, %qc);
	if (err)
		return err;
	struct irq *q = (struct irq *)qc->obj;
	if (!q->ntfn)
		return -ERR_INVAL;
	arch_irq_unmask(q->line);
	return 0;
}

void irq_release(struct irq *q)
{
	arch_irq_mask(q->line);
	uint64_t flags = irq_save();
	if (bound[q->line] == q)
		bound[q->line] = NULL;
	struct notification *n = q->ntfn;
	q->ntfn = NULL;
	irq_restore(flags);
	if (n)
		kobj_put(&n->obj);
}

int64_t sys_ioport_enable(struct cspace *cs, int64_t slot)
{
	struct cap *c;
	int64_t err = cspace_lookup(cs, slot, KOBJ_IOPORT, CAP_WRITE, &c);
	if (err)
		return err;
	struct ioport *io = (struct ioport *)c->obj;
	struct process *p = thread_current()->proc;

	uint64_t flags = irq_save();
	for (unsigned port = io->base; port < (unsigned)io->base + io->count; port++)
		p->io_allowed[port / 8] |= (uint8_t)(1u << (port % 8));
	p->has_io = true;
	arch_set_io_bitmap(p->io_allowed);
	irq_restore(flags);
	return 0;
}
