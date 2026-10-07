/* Slab allocator: one cache per kernel object type (thread, endpoint,
 * capability table, ...). Each cache hands out fixed-size objects carved
 * out of 4 KiB pages ("slabs"). Fixed sizes mean no fragmentation and no
 * way for user requests to make the kernel allocate odd-sized blocks. */
#pragma once
#include <stddef.h>
#include <stdint.h>

struct slab;

struct slab_cache {
    const char  *name;
    size_t       obj_size;       /* rounded up to a multiple of 16 */
    size_t       objs_per_slab;
    size_t       first_offset;   /* where the first object starts in a slab */
    struct slab *partial;        /* slabs with at least one free object */
    struct slab *full;           /* slabs with no free objects */
    uint64_t     slabs;          /* slabs currently allocated */
    uint64_t     in_use;         /* objects currently allocated */
};

/* `size` must be at most about 4000 bytes (one object per page). */
void slab_cache_init(struct slab_cache *cache, const char *name, size_t size);

void *slab_alloc(struct slab_cache *cache);   /* zeroed; NULL if out of memory */
void  slab_free(struct slab_cache *cache, void *obj);