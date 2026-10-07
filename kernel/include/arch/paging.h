/* Page tables. The interface is portable; the implementation is x86_64
 * 4-level paging (PML4 -> PDPT -> PD -> PT). An address space is named by
 * its "root": the physical address of its top-level table. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <kernel/boot.h>

/* Mapping permissions. Pages are always readable; without PAGE_EXEC the
 * CPU refuses to run code from them (NX, "no execute"). */
#define PAGE_WRITE    (1u << 0)
#define PAGE_EXEC     (1u << 1)
#define PAGE_USER     (1u << 2)
#define PAGE_UNCACHED (1u << 3)   /* for device registers (MMIO) */

/* Build the kernel's own page tables (HHDM + kernel image with per-section
 * permissions) and switch to them. Needs pmm_init() first. */
void paging_init(const struct boot_info *boot);

uint64_t paging_kernel_root(void);

/* New address space: empty lower (user) half, shared kernel upper half. */
uint64_t paging_new_root(void);
/* Free the page tables of a root made by paging_new_root(). Does not free
 * the frames that were mapped in it; their owner does that. */
void paging_destroy_root(uint64_t root);
void paging_activate(uint64_t root);

/* Map one 4 KiB page. Returns false if `virt` is already mapped or a page
 * table could not be allocated. */
bool paging_map(uint64_t root, uint64_t virt, uint64_t phys, unsigned flags);
/* Map a range, using 2 MiB pages where alignment allows. Pages already
 * mapped to the same physical address are skipped; a conflicting mapping
 * makes it return false. */
bool paging_map_range(uint64_t root, uint64_t virt, uint64_t phys,
                      uint64_t len, unsigned flags);
/* Unmap one 4 KiB page and flush it from the TLB (translation lookaside
 * buffer, the CPU's cache of translations). Returns the physical address
 * that was mapped, or 0 if nothing was. */
uint64_t paging_unmap(uint64_t root, uint64_t virt);
bool paging_translate(uint64_t root, uint64_t virt, uint64_t *phys_out);