/* Everything the kernel learns from the bootloader, copied into one struct
 * early so the rest of the kernel never depends on Limine directly. */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum mem_type {
    MEM_USABLE,
    MEM_RESERVED,
    MEM_ACPI_RECLAIMABLE,
    MEM_ACPI_NVS,
    MEM_BAD,
    MEM_BOOTLOADER_RECLAIMABLE,
    MEM_KERNEL_AND_MODULES,
    MEM_FRAMEBUFFER,
};

struct mem_region {
    uint64_t base;
    uint64_t length;
    enum mem_type type;
};

#define BOOT_MAX_REGIONS 128
#define BOOT_CMDLINE_MAX 256

struct boot_info {
    uint64_t hhdm_offset;          /* virt = phys + hhdm_offset for all RAM */
    uint64_t kernel_phys_base;
    uint64_t kernel_virt_base;
    char     cmdline[BOOT_CMDLINE_MAX];  /* "" if none was given */
    size_t   region_count;
    struct mem_region regions[BOOT_MAX_REGIONS];
};

/* Fills `out` from the bootloader's responses. Panics if a required
 * response is missing. */
void boot_info_collect(struct boot_info *out);
const char *mem_type_name(enum mem_type t);