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
#define BOOT_MAX_MODULES 8

/* A file the bootloader loaded next to the kernel (e.g. the root task).
 * It sits in MEM_KERNEL_AND_MODULES memory, which is never reclaimed. */
struct boot_module {
    char     name[64];   /* file name without directories, e.g. "root.elf" */
    uint64_t phys;
    uint64_t size;
};

struct boot_info {
    uint64_t hhdm_offset;          /* virt = phys + hhdm_offset for all RAM */
    uint64_t kernel_phys_base;
    uint64_t kernel_virt_base;
    char     cmdline[BOOT_CMDLINE_MAX];  /* "" if none was given */
    size_t   region_count;
    struct mem_region regions[BOOT_MAX_REGIONS];
    size_t   module_count;
    struct boot_module modules[BOOT_MAX_MODULES];
};

/* The module called `name`, or NULL. */
const struct boot_module *boot_find_module(const struct boot_info *boot, const char *name);

/* Fills `out` from the bootloader's responses. Panics if a required
 * response is missing. */
void boot_info_collect(struct boot_info *out);
const char *mem_type_name(enum mem_type t);