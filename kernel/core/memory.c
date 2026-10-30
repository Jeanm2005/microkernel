/* Frames, mappings and factory quotas. See kernel/memory.h.
 *
 * A frame is the only way user space gets memory beyond its program image
 * and stack. Its physical address is visible (SYS_FRAME_INFO) because a
 * device doing DMA (direct memory access) needs it; without an IOMMU
 * (I/O memory management unit) a DMA-capable driver can reach any memory
 * anyway, so it is trusted with the whole machine whatever we do here. */
#include <arch/cpu.h>
#include <arch/paging.h>
#include <abi/syscall.h>
#include <kernel/cap.h>
#include <kernel/memory.h>
#include <kernel/mm.h>
#include <kernel/object.h>
#include <kernel/pmm.h>
#include <kernel/process.h>
#include <kernel/sched.h>

int64_t sys_frame_alloc(struct cspace *cs, int64_t factory_slot, uint64_t pages)
{
    struct cap *fc;
    int64_t err = cspace_lookup(cs, factory_slot, KOBJ_FACTORY, CAP_WRITE, &fc);
    if (err)
        return err;
    if (pages == 0 || pages > FRAME_MAX_PAGES)
        return -ERR_INVAL;
    struct factory *f = (struct factory *)fc->obj;

    uint64_t flags = irq_save();
    bool enough = f->pages >= pages;
    if (enough)
        f->pages -= (uint32_t)pages;
    irq_restore(flags);
    if (!enough)
        return -ERR_NOMEM;

    struct frame *fr = (struct frame *)kobj_create(KOBJ_FRAME, f);
    uint64_t phys = fr ? pmm_alloc_contig(pages) : PMM_NONE;
    if (phys == PMM_NONE) {
        if (fr)
            kobj_put(&fr->obj);
        flags = irq_save();
        f->pages += (uint32_t)pages;
        irq_restore(flags);
        return -ERR_NOMEM;
    }
    fr->phys = phys;
    fr->pages = (uint32_t)pages;
    int64_t slot = cspace_insert(cs, &fr->obj, CAP_ALL, 0);
    kobj_put(&fr->obj);
    return slot;
}

int64_t sys_frame_info(struct cspace *cs, int64_t slot, uint64_t *pages)
{
    struct cap *c;
    int64_t err = cspace_lookup(cs, slot, KOBJ_FRAME, CAP_READ, &c);
    if (err)
        return err;
    struct frame *fr = (struct frame *)c->obj;
    *pages = fr->pages;
    return (int64_t)fr->phys;
}

int64_t sys_map(struct cspace *cs, int64_t slot, uint64_t vaddr, uint64_t writable)
{
    struct cap *c;
    int64_t err = cspace_lookup(cs, slot, KOBJ_FRAME, writable ? CAP_READ | CAP_WRITE : CAP_READ, &c);
    if (err)
        return err;
    struct frame *fr = (struct frame *)c->obj;
    uint64_t len = (uint64_t)fr->pages * PAGE_SIZE;
    if (!IS_ALIGNED(vaddr, PAGE_SIZE) || vaddr < USER_MAP_BASE || vaddr > USER_MAP_END - len)
        return -ERR_INVAL;

    struct process *p = thread_current()->proc;
    uint64_t saved = irq_save();
    if (!cap_node_valid(c->node)) { irq_restore(saved); return -ERR_REVOKED; }
    int m;
    for (m = 0; m < PROC_MAPPINGS && p->maps[m].frame; m++)
        ;
    if (m == PROC_MAPPINGS) { irq_restore(saved); return -ERR_FULL; }

    unsigned flags = PAGE_USER | (writable ? PAGE_WRITE : 0);
    for (uint64_t off = 0; off < len; off += PAGE_SIZE) {
        if (!paging_map(p->root, vaddr + off, fr->phys + off, flags)) {
            for (uint64_t undo = 0; undo < off; undo += PAGE_SIZE)
                paging_unmap(p->root, vaddr + undo);
            irq_restore(saved); return -ERR_EXIST;
        }
    }
    cap_use_get(c->node); p->maps[m].authority = c->node;
    kobj_get(&fr->obj);
    p->maps[m].vaddr = vaddr;
    p->maps[m].frame = fr;
    irq_restore(saved);
    return 0;
}

static void unmap_one(struct process *p, int m)
{
    uint64_t saved = irq_save();
    struct frame *fr = p->maps[m].frame;
    for (uint64_t off = 0; off < (uint64_t)fr->pages * PAGE_SIZE; off += PAGE_SIZE)
        paging_unmap(p->root, p->maps[m].vaddr + off);
    p->maps[m].frame = NULL;
    p->maps[m].vaddr = 0;
    kobj_put(&fr->obj);
    cap_use_put(p->maps[m].authority); p->maps[m].authority = NULL;
    irq_restore(saved);
}

int64_t sys_unmap(uint64_t vaddr)
{
    struct process *p = thread_current()->proc;
    for (int m = 0; m < PROC_MAPPINGS; m++) {
        if (p->maps[m].frame && p->maps[m].vaddr == vaddr) {
            unmap_one(p, m);
            return 0;
        }
    }
    return -ERR_INVAL;
}

void memory_unmap_all(struct process *p)
{
    for (int m = 0; m < PROC_MAPPINGS; m++)
        if (p->maps[m].frame)
            unmap_one(p, m);
}

int64_t sys_factory_split(struct cspace *cs, int64_t slot, uint64_t objects, uint64_t pages)
{
    struct cap *fc;
    int64_t err = cspace_lookup(cs, slot, KOBJ_FACTORY, CAP_WRITE, &fc);
    if (err)
        return err;
    struct factory *parent = (struct factory *)fc->obj;

    /* The new factory is itself one object, paid for by the parent; its
     * budget is carved out of the parent's on top of that. */
    uint64_t flags = irq_save();
    bool enough = parent->budget > objects && parent->pages >= pages;
    if (enough) {
        parent->budget -= (uint32_t)objects;
        parent->pages -= (uint32_t)pages;
    }
    irq_restore(flags);
    if (!enough)
        return -ERR_NOMEM;

    struct factory *child = (struct factory *)kobj_create(KOBJ_FACTORY, parent);
    if (!child) {
        flags = irq_save();
        parent->budget += (uint32_t)objects;
        parent->pages += (uint32_t)pages;
        irq_restore(flags);
        return -ERR_NOMEM;
    }
    child->budget = (uint32_t)objects;
    child->pages = (uint32_t)pages;
    int64_t new_slot = cspace_insert(cs, &child->obj, CAP_ALL, 0);
    kobj_put(&child->obj);
    return new_slot;
}