#include <arch/cpu.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/object.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/process.h>
#include <kernel/slab.h>
#include <kernel/string.h>

static struct slab_cache endpoint_cache, notification_cache, factory_cache,
                         module_cache, process_obj_cache, ioport_cache, irq_cache,
                         frame_cache, irq_control_cache;

void object_init(void)
{
    slab_cache_init(&endpoint_cache, "endpoint", sizeof(struct endpoint));
    slab_cache_init(&notification_cache, "notification", sizeof(struct notification));
    slab_cache_init(&factory_cache, "factory", sizeof(struct factory));
    slab_cache_init(&module_cache, "module", sizeof(struct module));
    slab_cache_init(&process_obj_cache, "process_obj", sizeof(struct process_obj));
    slab_cache_init(&ioport_cache, "ioport", sizeof(struct ioport));
    slab_cache_init(&irq_cache, "irq", sizeof(struct irq));
    slab_cache_init(&frame_cache, "frame", sizeof(struct frame));
    slab_cache_init(&irq_control_cache, "irq_control", sizeof(struct kobj));
}

static struct slab_cache *cache_for(uint32_t type)
{
    switch (type) {
    case KOBJ_ENDPOINT:     return &endpoint_cache;
    case KOBJ_NOTIFICATION: return &notification_cache;
    case KOBJ_FACTORY:      return &factory_cache;
    case KOBJ_MODULE:       return &module_cache;
    case KOBJ_PROCESS:      return &process_obj_cache;
    case KOBJ_IOPORT:       return &ioport_cache;
    case KOBJ_IRQ:          return &irq_cache;
    case KOBJ_FRAME:        return &frame_cache;
    case KOBJ_IRQ_CONTROL:  return &irq_control_cache;
    default:                return NULL;
    }
}

const char *kobj_type_name(uint32_t type)
{
    switch (type) {
    case KOBJ_ENDPOINT:     return "endpoint";
    case KOBJ_NOTIFICATION: return "notification";
    case KOBJ_FACTORY:      return "factory";
    case KOBJ_MODULE:       return "module";
    case KOBJ_PROCESS:      return "process";
    case KOBJ_IOPORT:       return "ioport";
    case KOBJ_IRQ:          return "irq";
    case KOBJ_FRAME:        return "frame";
    case KOBJ_IRQ_CONTROL:  return "irq_control";
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

struct kobj *factory_create(uint32_t objects, uint32_t pages)
{
    struct factory *f = (struct factory *)kobj_create(KOBJ_FACTORY, NULL);
    if (!f)
        return NULL;
    f->budget = objects;
    f->pages = pages;
    return &f->obj;
}

struct kobj *module_create(const char *name, const void *data, uint64_t size)
{
    struct module *m = (struct module *)kobj_create(KOBJ_MODULE, NULL);
    if (!m)
        return NULL;
    size_t n = strlen(name);
    if (n >= sizeof m->name)
        n = sizeof m->name - 1;
    memcpy(m->name, name, n);
    m->data = data;
    m->size = size;
    return &m->obj;
}

struct kobj *ioport_create(uint32_t base, uint32_t count, struct factory *from)
{
    struct ioport *io = (struct ioport *)kobj_create(KOBJ_IOPORT, from);
    if (!io)
        return NULL;
    io->base = base;
    io->count = count;
    return &io->obj;
}

struct kobj *irq_control_create(void)
{
    return kobj_create(KOBJ_IRQ_CONTROL, NULL);
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
        struct notification *n = (struct notification *)o;
        kassert(waitq_empty(&n->waiters) && !n->bound);   /* a bound thread holds a ref */
    } else if (o->type == KOBJ_IRQ) {
        irq_release((struct irq *)o);
    } else if (o->type == KOBJ_PROCESS) {
        struct process *p = ((struct process_obj *)o)->proc;
        if (p)
            process_handle_gone(p);
    }

    /* What goes back to the factory that paid: the object itself, plus a
     * frame's pages, plus whatever budget a split-off factory still has
     * (all of it, since everything it made is gone by now). */
    uint32_t objects = 1, pages = 0;
    if (o->type == KOBJ_FRAME) {
        struct frame *fr = (struct frame *)o;
        pmm_free_contig(fr->phys, fr->pages);
        pages = fr->pages;
    } else if (o->type == KOBJ_FACTORY) {
        objects += ((struct factory *)o)->budget;
        pages = ((struct factory *)o)->pages;
    }

    struct factory *origin = o->origin;
    slab_free(cache_for(o->type), o);
    if (origin) {
        flags = irq_save();
        origin->budget += objects;
        origin->pages += pages;
        irq_restore(flags);
        kobj_put(&origin->obj);
    }
}