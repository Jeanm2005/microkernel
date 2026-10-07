/* Slab allocator. Each slab is one 4 KiB frame, reached through the HHDM:
 *
 *   +-------------+-------+-------+-------+-----+
 *   | struct slab | obj 0 | obj 1 | obj 2 | ... |
 *   +-------------+-------+-------+-------+-----+
 *
 * Because the header sits at the start of the page, slab_free() finds an
 * object's slab by rounding its address down to the page. Free objects
 * form a linked list through their first word. */
#include <stdbool.h>
#include <kernel/kprintf.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/slab.h>
#include <kernel/string.h>

#define SLAB_MAGIC 0x51ab51ab
/* Written into the second word of every free object. Finding it there in
 * slab_free() means the object is already free: a double free. */
#define FREE_MAGIC 0xf4eef4eef4eef4eeull

struct free_obj {
    struct free_obj *next;
    uint64_t magic;
};

struct slab {
    struct slab_cache *cache;
    struct slab *prev, *next;   /* in cache->partial or cache->full */
    struct free_obj *free;
    uint32_t in_use;
    uint32_t magic;
};

static void list_remove(struct slab **head, struct slab *s)
{
    if (s->prev)
        s->prev->next = s->next;
    else
        *head = s->next;
    if (s->next)
        s->next->prev = s->prev;
    s->prev = s->next = NULL;
}

static void list_push(struct slab **head, struct slab *s)
{
    s->prev = NULL;
    s->next = *head;
    if (*head)
        (*head)->prev = s;
    *head = s;
}

void slab_cache_init(struct slab_cache *c, const char *name, size_t size)
{
    memset(c, 0, sizeof *c);
    c->name = name;
    c->obj_size = ALIGN_UP(size < 16 ? 16 : size, 16);
    c->first_offset = ALIGN_UP(sizeof(struct slab), 16);
    c->objs_per_slab = (PAGE_SIZE - c->first_offset) / c->obj_size;
    if (c->objs_per_slab == 0)
        panic("slab: objects of %zu bytes don't fit in a page", size);
}

static struct slab *new_slab(struct slab_cache *c)
{
    uint64_t frame = pmm_alloc();
    if (frame == PMM_NONE)
        return NULL;

    struct slab *s = phys_to_virt(frame);
    *s = (struct slab){ .cache = c, .magic = SLAB_MAGIC };

    /* Thread every object onto the free list, lowest address first. */
    uint8_t *base = (uint8_t *)s + c->first_offset;
    for (size_t i = c->objs_per_slab; i-- > 0;) {
        struct free_obj *o = (struct free_obj *)(base + i * c->obj_size);
        o->next = s->free;
        o->magic = FREE_MAGIC;
        s->free = o;
    }
    c->slabs++;
    return s;
}

void *slab_alloc(struct slab_cache *c)
{
    struct slab *s = c->partial;
    if (!s) {
        s = new_slab(c);
        if (!s)
            return NULL;
        list_push(&c->partial, s);
    }

    struct free_obj *o = s->free;
    kassert(o && o->magic == FREE_MAGIC);   /* a free object was overwritten? */
    s->free = o->next;
    s->in_use++;
    c->in_use++;

    if (!s->free) {
        list_remove(&c->partial, s);
        list_push(&c->full, s);
    }
    memset(o, 0, c->obj_size);
    return o;
}

void slab_free(struct slab_cache *c, void *obj)
{
    struct slab *s = (struct slab *)ALIGN_DOWN((uint64_t)obj, PAGE_SIZE);
    if (s->magic != SLAB_MAGIC || s->cache != c)
        panic("slab_free(%s): %p is not from this cache", c->name, obj);

    uint64_t offset = (uint64_t)obj - (uint64_t)s - c->first_offset;
    if ((uint64_t)obj < (uint64_t)s + c->first_offset || offset % c->obj_size != 0)
        panic("slab_free(%s): %p is not the start of an object", c->name, obj);

    struct free_obj *o = obj;
    if (o->magic == FREE_MAGIC)
        panic("slab_free(%s): double free of %p", c->name, obj);

    bool was_full = (s->free == NULL);
    o->next = s->free;
    o->magic = FREE_MAGIC;
    s->free = o;
    s->in_use--;
    c->in_use--;

    if (was_full) {
        list_remove(&c->full, s);
        list_push(&c->partial, s);
    }
    /* Give empty slabs straight back. Simple, but a cache that hovers
     * around a slab boundary will allocate and free the same page over and
     * over; keeping one empty slab per cache would fix that if it matters. */
    if (s->in_use == 0) {
        list_remove(&c->partial, s);
        s->magic = 0;
        c->slabs--;
        pmm_free(hhdm_to_phys(s));
    }
}