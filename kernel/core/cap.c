/* Capability tables. Only the owning process's thread changes its table,
 * except that IPC inserts received capabilities into a blocked receiver's
 * table; both happen with interrupts off, which is the lock for now. */
#include <arch/cpu.h>
#include <abi/syscall.h>
#include <kernel/cap.h>
#include <kernel/object.h>
#include <kernel/string.h>

int64_t cspace_insert(struct cspace *cs, struct kobj *obj, uint32_t rights, uint64_t badge)
{
    uint64_t flags = irq_save();
    for (int i = 1; i < CSPACE_SLOTS; i++) {
        if (!cs->slot[i].obj) {
            kobj_get(obj);
            cs->slot[i] = (struct cap){ .obj = obj, .rights = rights & CAP_ALL, .badge = badge };
            irq_restore(flags);
            return i;
        }
    }
    irq_restore(flags);
    return -ERR_FULL;
}

int64_t cspace_lookup(struct cspace *cs, int64_t slot, uint32_t type, uint32_t rights,
                      struct cap **out)
{
    if (slot <= 0 || slot >= CSPACE_SLOTS || !cs->slot[slot].obj)
        return -ERR_BADCAP;
    struct cap *c = &cs->slot[slot];
    if (type && c->obj->type != type)
        return -ERR_TYPE;
    if ((c->rights & rights) != rights)
        return -ERR_PERM;
    *out = c;
    return 0;
}

int64_t cspace_copy(struct cspace *cs, int64_t slot, uint32_t rights, uint64_t badge)
{
    struct cap *src;
    int64_t err = cspace_lookup(cs, slot, 0, 0, &src);
    if (err)
        return err;
    if (rights & ~src->rights)
        return -ERR_PERM;   /* a copy can never have more rights */
    if (badge && (src->obj->type != KOBJ_ENDPOINT && src->obj->type != KOBJ_NOTIFICATION))
        return -ERR_INVAL;
    if (badge && src->badge && badge != src->badge)
        return -ERR_PERM;   /* no re-badging */
    return cspace_insert(cs, src->obj, rights, badge ? badge : src->badge);
}

int64_t cspace_delete(struct cspace *cs, int64_t slot)
{
    struct cap *c;
    int64_t err = cspace_lookup(cs, slot, 0, 0, &c);
    if (err)
        return err;
    struct kobj *o = c->obj;
    memset(c, 0, sizeof *c);
    kobj_put(o);
    return 0;
}

void cspace_clear(struct cspace *cs)
{
    for (int i = 1; i < CSPACE_SLOTS; i++)
        if (cs->slot[i].obj)
            cspace_delete(cs, i);
}