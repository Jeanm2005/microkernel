/* Basic memory definitions shared by the whole kernel. */
#pragma once
#include <stdint.h>

#define PAGE_SIZE ((uint64_t)4096)
#define PAGE_SHIFT 12

#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))
#define ALIGN_UP(x, a) ALIGN_DOWN((x) + (a) - 1, (a))
#define IS_ALIGNED(x, a) (((x) & ((uint64_t)(a) - 1)) == 0)

/* The HHDM (higher-half direct map) maps all RAM at a fixed offset:
 * virtual = physical + hhdm_offset. Set by pmm_init(). */
 extern uint64_t hhdm_offset;

 static inline void *phys_to_virt(uint64_t phys)
 {
    return (void *)(phys + hhdm_offset);
 }

 /* Only valid for pointers into the HHDM (e.g. from phys_to_virt or the
 * slab allocator), not for addresses in the kernel image. */
 static inline uint64_t hhdm_to_phys(const void *virt)
 {
    return (uint64_t)virt - hhdm_offset;
 }