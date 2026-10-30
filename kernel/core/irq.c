/* Device interrupts and I/O ports. See kernel/irq.h. */
#include <arch/cpu.h>
#include <arch/ioport.h>
#include <arch/irq.h>
#include <abi/syscall.h>
#include <kernel/cap.h>
#include <kernel/ipc.h>
#include <kernel/irq.h>
#include <kernel/mm.h>
#include <kernel/object.h>
#include <kernel/pmm.h>
#include <kernel/process.h>
#include <kernel/sched.h>

static struct irq *owner[ARCH_IRQ_LINES];   /* who claimed each line */
static uint64_t delivered;

/* ---- interrupts --------------------------------------------------------- */

void irq_handle(unsigned line)
{
    struct irq *q = line < ARCH_IRQ_LINES ? owner[line] : NULL;
    if (!q || !q->ntfn)
        return;   /* nobody is listening: it stays masked */
    delivered++;
    ntfn_signal(q->ntfn, q->bits);
}

uint64_t irq_delivered(void)
{
    return delivered;
}

int64_t sys_irq_get(struct cspace *cs, int64_t factory_slot, int64_t control_slot, uint64_t line)
{
    struct cap *fc, *cc;
    int64_t err = cspace_lookup(cs, factory_slot, KOBJ_FACTORY, CAP_WRITE, &fc);
    if (!err)
        err = cspace_lookup(cs, control_slot, KOBJ_IRQ_CONTROL, CAP_WRITE, &cc);
    if (err)
        return err;
    /* Line 2 is where the second PIC cascades into the first; line 0 is
     * the old PIT timer, which the scheduler replaced with the LAPIC's. */
    if (line >= ARCH_IRQ_LINES || line == 0 || line == 2)
        return -ERR_INVAL;

    struct irq *q = (struct irq *)kobj_create(KOBJ_IRQ, (struct factory *)fc->obj);
    if (!q)
        return -ERR_NOMEM;
    q->line = (uint32_t)line;
    uint64_t flags = irq_save();
    bool taken = owner[line] != NULL;
    if (!taken)
        owner[line] = q;
    irq_restore(flags);
    if (taken) {
        q->line = ARCH_IRQ_LINES;   /* not ours: irq_release must leave it alone */
        kobj_put(&q->obj);
        return -ERR_EXIST;
    }
    int64_t slot = cspace_insert(cs, &q->obj, CAP_ALL, 0);
    kobj_put(&q->obj);   /* the capability holds the only reference */
    return slot;
}

int64_t sys_irq_bind(struct cspace *cs, int64_t irq_slot, int64_t ntfn_slot, uint64_t bits)
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
    irq_restore(flags);
    if (old)
        kobj_put(&old->obj);
    arch_irq_unmask(q->line);
    return 0;
}

int64_t sys_irq_ack(struct cspace *cs, int64_t irq_slot)
{
    struct cap *qc;
    int64_t err = cspace_lookup(cs, irq_slot, KOBJ_IRQ, CAP_WRITE, &qc);
    if (err)
        return err;
    struct irq *q = (struct irq *)qc->obj;
    if (!q->ntfn)
        return -ERR_INVAL;   /* not bound */
    arch_irq_unmask(q->line);
    return 0;
}

void irq_release(struct irq *q)
{
    if (q->line >= ARCH_IRQ_LINES)
        return;   /* never got its line */
    arch_irq_mask(q->line);
    uint64_t flags = irq_save();
    if (owner[q->line] == q)
        owner[q->line] = NULL;
    struct notification *n = q->ntfn;
    q->ntfn = NULL;
    irq_restore(flags);
    if (n)
        kobj_put(&n->obj);
}

/* ---- I/O ports ---------------------------------------------------------- */

#define IO_BITMAP_BYTES (ARCH_IO_PORTS / 8)   /* 8 KiB */
#define IO_BITMAP_PAGES (IO_BITMAP_BYTES / PAGE_SIZE)

int64_t sys_ioport_enable(struct cspace *cs, int64_t slot)
{
    struct cap *c;
    int64_t err = cspace_lookup(cs, slot, KOBJ_IOPORT, CAP_WRITE, &c);
    if (err)
        return err;
    struct ioport *io = (struct ioport *)c->obj;
    struct process *p = thread_current()->proc;

    /* Most processes have no ports, so the 8 KiB bitmap is only allocated
     * when a process enables its first one. */
    if (!p->io_allowed) {
        uint64_t phys = pmm_alloc_contig(IO_BITMAP_PAGES);   /* zeroed: nothing allowed */
        if (phys == PMM_NONE)
            return -ERR_NOMEM;
        p->io_allowed = phys_to_virt(phys);
        p->io_lo = IO_BITMAP_BYTES;
        p->io_hi = 0;
    }

    /* The permission belongs to the process from now on; deleting the
     * capability doesn't take it back (that needs revocation, M9). */
    uint64_t flags = irq_save();
    for (uint32_t port = io->base; port < io->base + io->count; port++)
        p->io_allowed[port / 8] |= (uint8_t)(1u << (port % 8));
    if (io->base / 8 < p->io_lo)
        p->io_lo = io->base / 8;
    if ((io->base + io->count + 7) / 8 > p->io_hi)
        p->io_hi = (io->base + io->count + 7) / 8;
    arch_set_io_bitmap(p->io_allowed, p->io_lo, p->io_hi);   /* takes effect right away */
    irq_restore(flags);
    return 0;
}

int64_t sys_ioport_sub(struct cspace *cs, int64_t factory_slot, int64_t slot, uint64_t base,
                       uint64_t count)
{
    struct cap *fc, *c;
    int64_t err = cspace_lookup(cs, factory_slot, KOBJ_FACTORY, CAP_WRITE, &fc);
    if (!err)
        err = cspace_lookup(cs, slot, KOBJ_IOPORT, CAP_WRITE, &c);
    if (err)
        return err;
    struct ioport *io = (struct ioport *)c->obj;
    if (count == 0 || base < io->base || base + count > (uint64_t)io->base + io->count)
        return -ERR_INVAL;   /* only a part of what we hold */
    struct kobj *sub = ioport_create((uint32_t)base, (uint32_t)count, (struct factory *)fc->obj);
    if (!sub)
        return -ERR_NOMEM;
    int64_t new_slot = cspace_insert(cs, sub, CAP_ALL, 0);
    kobj_put(sub);
    return new_slot;
}

void ioport_release(struct process *p)
{
    if (p->io_allowed) {
        pmm_free_contig(hhdm_to_phys(p->io_allowed), IO_BITMAP_PAGES);
        p->io_allowed = NULL;
    }
}
int64_t sys_irq_unbind(struct cspace *cs, int64_t slot)
{
    struct cap *c;
    int64_t err = cspace_lookup(cs, slot, KOBJ_IRQ, CAP_WRITE, &c);
    if (err) return err;
    struct irq *q = (struct irq *)c->obj;
    uint64_t flags = irq_save();
    arch_irq_mask(q->line);
    struct notification *old = q->ntfn;
    q->ntfn = NULL;
    q->bits = 0;
    irq_restore(flags);
    if (old) kobj_put(&old->obj);
    return 0;
}
