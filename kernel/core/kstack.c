/* Kernel stack allocator. Stacks live in their own 128 MiB virtual
 * region, split into fixed 32 KiB slots:
 *
 *   slot base                                   slot base + 32 KiB
 *   |  guard: 16 KiB, never mapped  |  stack: 16 KiB, mapped  |
 *                                                             ^ top
 *
 * Overflowing a stack runs into the guard of its own slot, which faults.
 * The frames don't need to be physically contiguous since they're mapped
 * page by page. */
#include <stdbool.h>
#include <arch/cpu.h>
#include <arch/paging.h>
#include <kernel/kstack.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>

#define REGION_BASE  0xffffff0000000000ull   /* PML4 slot 510, unused otherwise */
#define SLOT_SIZE    (2 * KSTACK_SIZE)
#define MAX_SLOTS    4096
#define STACK_PAGES  (KSTACK_SIZE / PAGE_SIZE)

static uint64_t used[MAX_SLOTS / 64];   /* one bit per slot */

static uint64_t slot_top(unsigned slot)
{
    return REGION_BASE + (uint64_t)(slot + 1) * SLOT_SIZE;
}

static void unmap_stack(uint64_t top, unsigned pages)
{
    for (unsigned i = 0; i < pages; i++) {
        uint64_t frame = paging_unmap(paging_kernel_root(), top - (i + 1) * PAGE_SIZE);
        kassert(frame != 0);
        pmm_free(frame);
    }
}

uint64_t kstack_alloc(void)
{
    uint64_t flags = irq_save();
    int slot = -1;
    for (unsigned w = 0; w < MAX_SLOTS / 64 && slot < 0; w++)
        if (used[w] != ~0ull) {
            slot = w * 64 + __builtin_ctzll(~used[w]);
            used[w] |= 1ull << (slot % 64);
        }
    irq_restore(flags);
    if (slot < 0)
        return 0;

    uint64_t top = slot_top(slot);
    for (unsigned i = 0; i < STACK_PAGES; i++) {
        uint64_t frame = pmm_alloc();
        if (frame == PMM_NONE ||
            !paging_map(paging_kernel_root(), top - (i + 1) * PAGE_SIZE, frame, PAGE_WRITE)) {
            if (frame != PMM_NONE)
                pmm_free(frame);
            unmap_stack(top, i);          /* undo the pages mapped so far */
            flags = irq_save();
            used[slot / 64] &= ~(1ull << (slot % 64));
            irq_restore(flags);
            return 0;
        }
    }
    return top;
}

void kstack_free(uint64_t top)
{
    if (top == 0)
        return;
    kassert(top > REGION_BASE && (top - REGION_BASE) % SLOT_SIZE == 0);
    unsigned slot = (top - REGION_BASE) / SLOT_SIZE - 1;
    kassert(slot < MAX_SLOTS && (used[slot / 64] & (1ull << (slot % 64))));

    unmap_stack(top, STACK_PAGES);
    uint64_t flags = irq_save();
    used[slot / 64] &= ~(1ull << (slot % 64));
    irq_restore(flags);
}