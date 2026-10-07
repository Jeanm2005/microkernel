/* Physical memory manager: one bit per 4 KiB frame (1 = in use).
 *
 * Simple and good enough for now: allocation scans for a zero bit starting
 * where the last search stopped, 64 frames per step. A buddy allocator
 * would be needed for multi-frame contiguous allocations (DMA buffers);
 * we'll add one when a driver needs it. */
#include <stdbool.h>
#include <kernel/kprintf.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/string.h>

uint64_t hhdm_offset;

static uint64_t *bitmap;        /* in HHDM */
static uint64_t bitmap_words;
static uint64_t total_frames;   /* frames covered by the bitmap */
static uint64_t free_frames;
static uint64_t next_word;      /* where the next search starts */

static bool test_bit(uint64_t f) { return bitmap[f / 64] & (1ull << (f % 64)); }
static void set_bit(uint64_t f)  { bitmap[f / 64] |= 1ull << (f % 64); }
static void clear_bit(uint64_t f) { bitmap[f / 64] &= ~(1ull << (f % 64)); }

static void free_range(uint64_t base, uint64_t len)
{
    uint64_t first = ALIGN_UP(base, PAGE_SIZE) / PAGE_SIZE;
    uint64_t end = ALIGN_DOWN(base + len, PAGE_SIZE) / PAGE_SIZE;
    for (uint64_t f = first; f < end; f++) {
        if (f == 0)
            continue;   /* frame 0 is never handed out: 0 means PMM_NONE */
        if (test_bit(f)) {
            clear_bit(f);
            free_frames++;
        }
    }
}

static void reserve_range(uint64_t base, uint64_t len)
{
    uint64_t first = ALIGN_DOWN(base, PAGE_SIZE) / PAGE_SIZE;
    uint64_t end = ALIGN_UP(base + len, PAGE_SIZE) / PAGE_SIZE;
    for (uint64_t f = first; f < end && f < total_frames; f++) {
        if (!test_bit(f)) {
            set_bit(f);
            free_frames--;
        }
    }
}

void pmm_init(const struct boot_info *boot)
{
    hhdm_offset = boot->hhdm_offset;

    /* The bitmap covers everything up to the end of the highest region we
     * might ever hand out: usable RAM now, bootloader-reclaimable RAM once
     * we stop using Limine's data (M3). */
    uint64_t top = 0;
    for (size_t i = 0; i < boot->region_count; i++) {
        const struct mem_region *r = &boot->regions[i];
        if (r->type == MEM_USABLE || r->type == MEM_BOOTLOADER_RECLAIMABLE)
            if (r->base + r->length > top)
                top = r->base + r->length;
    }
    total_frames = top / PAGE_SIZE;
    bitmap_words = (total_frames + 63) / 64;
    uint64_t bitmap_bytes = bitmap_words * 8;

    /* Put the bitmap in the first usable region big enough to hold it. */
    uint64_t bitmap_phys = 0;
    for (size_t i = 0; i < boot->region_count; i++) {
        const struct mem_region *r = &boot->regions[i];
        uint64_t base = ALIGN_UP(r->base, PAGE_SIZE);
        if (r->type == MEM_USABLE && base != 0 &&
            r->base + r->length >= base + bitmap_bytes) {
            bitmap_phys = base;
            break;
        }
    }
    if (!bitmap_phys)
        panic("pmm: no region can hold the %lu-byte frame bitmap", bitmap_bytes);
    bitmap = phys_to_virt(bitmap_phys);

    /* Start with everything in use (including padding bits past the last
     * frame), then free the usable regions, then take back the bitmap. */
    memset(bitmap, 0xff, bitmap_bytes);
    free_frames = 0;
    for (size_t i = 0; i < boot->region_count; i++)
        if (boot->regions[i].type == MEM_USABLE)
            free_range(boot->regions[i].base, boot->regions[i].length);
    reserve_range(bitmap_phys, bitmap_bytes);

    kprintf("pmm: %lu frames tracked, %lu free (%lu MiB), bitmap %lu KiB at %p\n",
            total_frames, free_frames, (free_frames * PAGE_SIZE) >> 20,
            bitmap_bytes / 1024, (void *)bitmap_phys);
}

uint64_t pmm_alloc(void)
{
    for (uint64_t n = 0; n < bitmap_words; n++) {
        uint64_t w = (next_word + n) % bitmap_words;
        if (bitmap[w] != ~0ull) {
            uint64_t bit = (uint64_t)__builtin_ctzll(~bitmap[w]);
            uint64_t frame = w * 64 + bit;
            set_bit(frame);
            free_frames--;
            next_word = w;
            return frame * PAGE_SIZE;
        }
    }
    return PMM_NONE;
}

uint64_t pmm_alloc_zeroed(void)
{
    uint64_t f = pmm_alloc();
    if (f != PMM_NONE)
        memset(phys_to_virt(f), 0, PAGE_SIZE);
    return f;
}

void pmm_free(uint64_t frame)
{
    kassert(IS_ALIGNED(frame, PAGE_SIZE));
    uint64_t f = frame / PAGE_SIZE;
    kassert(f != 0 && f < total_frames);
    if (!test_bit(f))
        panic("pmm: double free of frame %p", (void *)frame);
    clear_bit(f);
    free_frames++;
}

uint64_t pmm_free_frames(void) { return free_frames; }
uint64_t pmm_total_frames(void) { return total_frames; }