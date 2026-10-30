/* Capability tables. Only the owning process's thread changes its table,
 * except that IPC inserts received capabilities into a blocked receiver's
 * table; both happen with interrupts off, which is the lock for now. */
#include <arch/cpu.h>
#include <abi/syscall.h>
#include <kernel/cap.h>
#include <kernel/object.h>
#include <kernel/slab.h>
#include <kernel/ipc.h>
#include <kernel/string.h>

static struct slab_cache nodes_cache;
static struct cap_node *nodes;
static unsigned node_count;
static int nodes_ready;

struct cap_node *cap_node_new(struct kobj *obj, struct cap_node *parent)
{
    if (node_count == 4096 || (parent && parent->depth >= 32)) return NULL;
    if (!nodes_ready) { slab_cache_init(&nodes_cache, "cap-node", sizeof(struct cap_node)); nodes_ready = 1; }
    struct cap_node *n = slab_alloc(&nodes_cache);
    if (!n) return NULL;
    n->obj = obj; kobj_get(obj);
    n->refs = 1; n->parent = parent;
    if (parent) { cap_node_get(parent); n->depth = parent->depth + 1; n->parent_generation = parent->generation; }
    n->next = nodes; nodes = n; node_count++;
    return n;
}
void cap_node_get(struct cap_node *n) { uint64_t flags = irq_save(); n->refs++; irq_restore(flags); }
void cap_node_put(struct cap_node *n)
{
    uint64_t flags = irq_save();
    if (--n->refs) { irq_restore(flags); return; }
    struct cap_node **link = &nodes;
    while (*link != n) link = &(*link)->next;
    *link = n->next; node_count--;
    struct cap_node *parent = n->parent;
    kobj_put(n->obj);
    slab_free(&nodes_cache, n);
    if (parent) cap_node_put(parent);
    irq_restore(flags);
}
int cap_node_valid(const struct cap_node *n)
{
    for (; n->parent; n = n->parent)
        if (n->parent_generation != n->parent->generation) return 0;
    return 1;
}
void cap_use_get(struct cap_node *n) { uint64_t f = irq_save(); cap_node_get(n); n->users++; irq_restore(f); }
void cap_use_put(struct cap_node *n) { uint64_t f = irq_save(); n->users--; cap_node_put(n); irq_restore(f); }
int64_t cspace_insert_node(struct cspace *cs, struct cap_node *node, uint32_t rights,
                           uint64_t badge, int64_t dst)
{
    if (!cap_node_valid(node)) return -ERR_REVOKED;
    if (dst < 0 || dst >= CSPACE_SLOTS) return -ERR_INVAL;
    if (dst && cs->slot[dst].obj) return -ERR_EXIST;
    if (!dst) {
        for (dst = 1; dst < CSPACE_SLOTS && cs->slot[dst].obj; dst++);
        if (dst == CSPACE_SLOTS) return -ERR_FULL;
    }
    cap_node_get(node);
    cs->slot[dst] = (struct cap){ .node = node, .obj = node->obj, .rights = rights & CAP_ALL, .badge = badge };
    return dst;
}
static int64_t insert_new(struct cspace *cs, int64_t slot, struct kobj *obj,
                         uint32_t rights, uint64_t badge)
{
    uint64_t flags = irq_save();
    if (slot < 0 || slot >= CSPACE_SLOTS) { irq_restore(flags); return -ERR_INVAL; }
    struct cap_node *n = cap_node_new(obj, NULL);
    int64_t result = n ? cspace_insert_node(cs, n, rights, badge, slot) : -ERR_NOMEM;
    if (n) cap_node_put(n);
    irq_restore(flags);
    return result;
}
int64_t cspace_insert_at(struct cspace *cs, int64_t slot, struct kobj *obj,
                         uint32_t rights, uint64_t badge)
{
    if (slot <= 0) return -ERR_INVAL;
    return insert_new(cs, slot, obj, rights, badge);
}
int64_t cspace_insert(struct cspace *cs, struct kobj *obj, uint32_t rights, uint64_t badge)
{
    return insert_new(cs, 0, obj, rights, badge);
}

int64_t cspace_lookup(struct cspace *cs, int64_t slot, uint32_t type, uint32_t rights,
                      struct cap **out)
{
    if (slot <= 0 || slot >= CSPACE_SLOTS || !cs->slot[slot].obj)
        return -ERR_BADCAP;
    struct cap *c = &cs->slot[slot];
    if (!cap_node_valid(c->node)) return -ERR_REVOKED;
    if (type && c->obj->type != type)
        return -ERR_TYPE;
    if ((c->rights & rights) != rights)
        return -ERR_PERM;
    *out = c;
    return 0;
}

int64_t cspace_derive(struct cspace *src, int64_t slot, struct cspace *dst, int64_t dst_slot,
                      uint32_t rights, uint64_t badge)
{
    struct cap *c;
    int64_t err = cspace_lookup(src, slot, 0, 0, &c);
    if (err)
        return err;
    if (rights & ~c->rights)
        return -ERR_PERM;   /* a copy can never have more rights */
    if (badge && (c->obj->type != KOBJ_ENDPOINT && c->obj->type != KOBJ_NOTIFICATION))
        return -ERR_INVAL;
    if (badge && c->badge && badge != c->badge)
        return -ERR_PERM;   /* no re-badging */
    uint64_t flags = irq_save();
    struct cap_node *n = cap_node_new(c->obj, c->node);
    int64_t result = n ? cspace_insert_node(dst, n, rights, badge ? badge : c->badge, dst_slot) : -ERR_NOMEM;
    if (n) cap_node_put(n);
    irq_restore(flags);
    return result;
}

int64_t cspace_copy(struct cspace *cs, int64_t slot, uint32_t rights, uint64_t badge)
{
    return cspace_derive(cs, slot, cs, 0, rights, badge);
}

int64_t cspace_delete(struct cspace *cs, int64_t slot)
{
    uint64_t flags = irq_save();
    if (slot <= 0 || slot >= CSPACE_SLOTS || !cs->slot[slot].obj) {
        irq_restore(flags); return -ERR_BADCAP;
    }
    struct cap_node *n = cs->slot[slot].node;
    memset(&cs->slot[slot], 0, sizeof cs->slot[slot]);
    cap_node_put(n);
    irq_restore(flags);
    return 0;
}
int64_t cspace_revoke(struct cspace *cs, int64_t slot)
{
    uint64_t flags = irq_save();
    struct cap *c;
    int64_t err = cspace_lookup(cs, slot, 0, CAP_GRANT, &c);
    if (err) { irq_restore(flags); return err; }
    /* Refuse live resources transactionally. Never silently leave an old
     * mapping or port bitmap authorized after reporting revocation. */
    for (struct cap_node *n = nodes; n; n = n->next) {
        int busy = n->users || (n->obj->type == KOBJ_IRQ && ((struct irq *)n->obj)->ntfn);
        if (!busy) continue;
        for (struct cap_node *a = n->parent; a; a = a->parent)
            if (a == c->node) { irq_restore(flags); return -ERR_BUSY; }
    }
    if (c->node->generation == UINT64_MAX) { irq_restore(flags); return -ERR_FULL; }
    c->node->generation++;
    ipc_revoke();
    irq_restore(flags);
    return 0;
}

void cspace_clear(struct cspace *cs)
{
    for (int i = 1; i < CSPACE_SLOTS; i++)
        if (cs->slot[i].obj)
            cspace_delete(cs, i);
}