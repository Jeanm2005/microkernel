#include <arch/cpu.h>
#include <kernel/kprintf.h>
#include <kernel/object.h>
#include <kernel/panic.h>
#include <kernel/slab.h>

static struct slab_cache endpoint_cache, notification_cache, factory_cache;

void object_init(void)
{
    slab_cache_init(&endpoint_cache, "endpoint", sizeof(struct endpoint));
    slab_cache_init(&notification_cache, "notification", sizeof(struct notification));
    slab_cache_init(&factory_cache, "factory", sizeof(struct factory));
}

static struct slab_cache *cache_for(uint32_t type)
{
    switch (type) {
    case KOBJ_ENDPOINT:     return &endpoint_cache;
    case KOBJ_NOTIFICATION: return &notification_cache;
    case KOBJ_FACTORY:      return &factory_cache;
    default:                return NULL;
    }
}

const char *kobj_type_name(uint32_t type)
{
    switch (type) {
    case KOBJ_ENDPOINT:     return "endpoint";
    case KOBJ_NOTIFICATION: return "notification";
    case KOBJ_FACTORY:      return "factory";
    default:                return "?";
    }
}

struct kobj *kobj_create(enum kobj_type type, struct factory *from)
{
    struct slab_cache *cache = cache_for(type);
    if (!cache)
        return NULL;

    uint64_t flags = irq_save();
    if (from && from->budget == 0) {
        irq_restore(flags);
        return NULL;
    }
    if (from) {
        from->budget--;
        from->obj.refs++;   /* the factory lives as long as its objects */
    }
    irq_restore(flags);

    struct kobj *o = slab_alloc(cache);   /* zeroed: empty queues, word 0 */
    if (!o) {
        if (from) {
            flags = irq_save();
            from->budget++;
            irq_restore(flags);
            kobj_put(&from->obj);
        }
        return NULL;
    }
    o->type = type;
    o->refs = 1;
    o->origin = from;
    return o;
}

struct kobj *factory_create(uint32_t budget)
{
    struct factory *f = (struct factory *)kobj_create(KOBJ_FACTORY, NULL);
    if (!f)
        return NULL;
    f->budget = budget;
    return &f->obj;
}

void kobj_get(struct kobj *o)
{
    uint64_t flags = irq_save();
    kassert(o->refs > 0);
    o->refs++;
    irq_restore(flags);
}

void kobj_put(struct kobj *o)
{
    uint64_t flags = irq_save();
    kassert(o->refs > 0);
    bool last = --o->refs == 0;
    irq_restore(flags);
    if (!last)
        return;

    /* Blocked threads hold references, so nobody can be waiting here. */
    if (o->type == KOBJ_ENDPOINT) {
        struct endpoint *ep = (struct endpoint *)o;
        kassert(waitq_empty(&ep->senders) && waitq_empty(&ep->receivers));
    } else if (o->type == KOBJ_NOTIFICATION) {
        kassert(waitq_empty(&((struct notification *)o)->waiters));
    }

    struct factory *origin = o->origin;
    slab_free(cache_for(o->type), o);
    if (origin) {
        flags = irq_save();
        origin->budget++;
        irq_restore(flags);
        kobj_put(&origin->obj);
    }
}