/* PMM (physical memory manager): hands out and takes back 4 KiB frames of
 * physical RAM. Frames are identified by physical address. */
#pragma once
#include <stdint.h>
#include <kernel/boot.h>

/* Returned by the allocators when no memory is left. Frame 0 is never
 * handed out, so 0 is safe to use as "none". */
#define PMM_NONE 0

void pmm_init(const struct boot_info *boot);

/* Hand the bootloader's leftover memory (its page tables, stack and boot
 * data) to the allocator. Only safe once nothing uses it any more: after
 * the switch to our own page tables and our own stacks. */
void pmm_reclaim_bootloader(const struct boot_info *boot);

uint64_t pmm_alloc(void);          /* contents undefined */
uint64_t pmm_alloc_zeroed(void);
void     pmm_free(uint64_t frame);

uint64_t pmm_free_frames(void);
uint64_t pmm_total_frames(void);