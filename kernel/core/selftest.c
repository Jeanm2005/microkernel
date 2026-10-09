/* Memory-management self-tests. The boot tests run on every boot and are
 * cheap; they catch allocator and page-table bugs the moment they appear
 * instead of three milestones later. */
#include <arch/paging.h>
#include <kernel/kprintf.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/selftest.h>
#include <kernel/slab.h>
#include <kernel/string.h>

/* An address in the user half that nothing uses yet. */
#define TEST_VADDR 0x400000ull

static void test_pmm(void)
{
    enum { N = 64 };
    uint64_t frames[N];
    uint64_t before = pmm_free_frames();

    for (int i = 0; i < N; i++) {
        frames[i] = pmm_alloc();
        kassert(frames[i] != PMM_NONE && IS_ALIGNED(frames[i], PAGE_SIZE));
        *(uint64_t *)phys_to_virt(frames[i]) = 0xf00d0000 + i;
        for (int j = 0; j < i; j++)
            kassert(frames[j] != frames[i]);
    }
    kassert(pmm_free_frames() == before - N);
    for (int i = 0; i < N; i++) {
        kassert(*(uint64_t *)phys_to_virt(frames[i]) == 0xf00d0000ull + i);
        pmm_free(frames[i]);
    }
    kassert(pmm_free_frames() == before);
    kprintf("selftest: pmm ok (%d frames)\n", N);
}

static void test_paging(void)
{
    uint64_t before = pmm_free_frames();
    uint64_t frame = pmm_alloc_zeroed();
    kassert(frame != PMM_NONE);

    /* 1. Map into the kernel's address space, write through the new
     *    mapping, read back through the HHDM: same physical memory. */
    uint64_t kroot = paging_kernel_root();
    kassert(paging_map(kroot, TEST_VADDR, frame, PAGE_WRITE));
    kassert(!paging_map(kroot, TEST_VADDR, frame, PAGE_WRITE));   /* no double map */
    uint64_t pa;
    kassert(paging_translate(kroot, TEST_VADDR + 0x123, &pa) && pa == frame + 0x123);
    *(volatile uint64_t *)TEST_VADDR = 0xabcdef;
    kassert(*(uint64_t *)phys_to_virt(frame) == 0xabcdef);
    kassert(paging_unmap(kroot, TEST_VADDR) == frame);
    kassert(!paging_translate(kroot, TEST_VADDR, NULL));

    /* 2. A separate address space: the same virtual address maps there and
     *    nowhere else, and the kernel half still works after switching. */
    uint64_t root = paging_new_root();
    kassert(root != PMM_NONE);
    kassert(paging_map(root, TEST_VADDR, frame, PAGE_WRITE));
    paging_activate(root);
    *(volatile uint64_t *)TEST_VADDR = 0x5ca1ab1e;   /* user half, new space */
    kprintf("selftest: running in a second address space\n"); /* kernel half */
    paging_activate(kroot);
    kassert(!paging_translate(kroot, TEST_VADDR, NULL));
    kassert(*(uint64_t *)phys_to_virt(frame) == 0x5ca1ab1e);
    kassert(paging_unmap(root, TEST_VADDR) == frame);
    paging_destroy_root(root, false);   /* frame is ours; freed below */

    pmm_free(frame);
    /* Page tables created for TEST_VADDR in the kernel root stay (they are
     * reused next time), so allow for those. */
    kassert(before - pmm_free_frames() <= 3);
    kprintf("selftest: paging ok\n");
}

struct test_obj {
    uint64_t id;
    uint8_t payload[40];   /* 48 bytes total: not a power of two */
};

static void test_slab(void)
{
    enum { N = 500 };
    static struct slab_cache cache;
    static struct test_obj *objs[N];
    uint64_t before = pmm_free_frames();

    slab_cache_init(&cache, "test", sizeof(struct test_obj));
    for (int i = 0; i < N; i++) {
        objs[i] = slab_alloc(&cache);
        kassert(objs[i] && IS_ALIGNED((uint64_t)objs[i], 16));
        kassert(objs[i]->id == 0);                 /* zeroed */
        objs[i]->id = i;
        memset(objs[i]->payload, i & 0xff, sizeof objs[i]->payload);
    }
    kassert(cache.in_use == N && cache.slabs == (N + cache.objs_per_slab - 1) / cache.objs_per_slab);

    /* Nothing overlapped: every object still holds what we wrote. */
    for (int i = 0; i < N; i++)
        kassert(objs[i]->id == (uint64_t)i && objs[i]->payload[39] == (i & 0xff));

    for (int i = 1; i < N; i += 2)                 /* free odd, reuse */
        slab_free(&cache, objs[i]);
    for (int i = 1; i < N; i += 2)
        kassert((objs[i] = slab_alloc(&cache)) != NULL);
    for (int i = 0; i < N; i++)
        slab_free(&cache, objs[i]);

    kassert(cache.in_use == 0 && cache.slabs == 0);
    kassert(pmm_free_frames() == before);          /* every slab returned */
    kprintf("selftest: slab ok (%d objects of %zu bytes, %zu per slab)\n",
            N, cache.obj_size, cache.objs_per_slab);
}

void core_selftest_boot(void)
{
    test_pmm();
    test_paging();
    test_slab();
}

bool core_selftest_run(const char *name)
{
    if (strcmp(name, "slab-doublefree") == 0) {
        static struct slab_cache cache;
        slab_cache_init(&cache, "doublefree", 32);
        void *keep = slab_alloc(&cache);   /* keeps the slab alive, so the */
        void *p = slab_alloc(&cache);      /* second free hits a real slab */
        kassert(keep && p);
        kprintf("selftest: freeing %p twice\n", p);
        slab_free(&cache, p);
        slab_free(&cache, p);
        panic("selftest '%s' returned; it should have crashed", name);
    }
    return false;
}