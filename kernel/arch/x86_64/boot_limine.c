/* The only file that knows about the Limine boot protocol. It declares the
 * requests Limine fills in before jumping to kmain(), then translates the
 * responses into the bootloader-neutral struct boot_info. */
#include <limine.h>
#include <kernel/boot.h>
#include <kernel/panic.h>
#include <kernel/string.h>

#define REQ __attribute__((used, section(".limine_requests"))) static volatile

/* Base revision 3 is supported by Limine v8+ (we ship v9). */
REQ uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(3);

REQ struct limine_memmap_request memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0,
};
REQ struct limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0,
};
REQ struct limine_executable_address_request kaddr_req = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0,
};
REQ struct limine_executable_cmdline_request cmdline_req = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0,
};
REQ struct limine_module_request module_req = {
    .id = LIMINE_MODULE_REQUEST_ID, .revision = 0,
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start[] = LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[] = LIMINE_REQUESTS_END_MARKER;

static enum mem_type translate(uint64_t t)
{
    switch (t) {
    case LIMINE_MEMMAP_USABLE:                 return MEM_USABLE;
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return MEM_ACPI_RECLAIMABLE;
    case LIMINE_MEMMAP_ACPI_NVS:               return MEM_ACPI_NVS;
    case LIMINE_MEMMAP_BAD_MEMORY:             return MEM_BAD;
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return MEM_BOOTLOADER_RECLAIMABLE;
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return MEM_KERNEL_AND_MODULES;
    case LIMINE_MEMMAP_FRAMEBUFFER:            return MEM_FRAMEBUFFER;
    default:                                   return MEM_RESERVED;
    }
}

void boot_info_collect(struct boot_info *out)
{
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision))
        panic("bootloader does not support Limine base revision 3");

    struct limine_memmap_response *mm = memmap_req.response;
    struct limine_hhdm_response *hh = hhdm_req.response;
    struct limine_executable_address_response *ka = kaddr_req.response;
    if (!mm || !hh || !ka)
        panic("missing Limine response (memmap=%p hhdm=%p kaddr=%p)",
              (void *)mm, (void *)hh, (void *)ka);

    out->hhdm_offset = hh->offset;
    out->kernel_phys_base = ka->physical_base;
    out->kernel_virt_base = ka->virtual_base;

    /* The command line is optional: no response means an empty one. */
    out->cmdline[0] = '\0';
    struct limine_executable_cmdline_response *cl = cmdline_req.response;
    if (cl && cl->cmdline) {
        size_t n = strlen(cl->cmdline);
        if (n >= BOOT_CMDLINE_MAX)
            n = BOOT_CMDLINE_MAX - 1;
        memcpy(out->cmdline, cl->cmdline, n);
        out->cmdline[n] = '\0';
    }

    /* Modules are optional too. Limine gives HHDM addresses; we keep
     * physical ones, like everything else in boot_info. */
    out->module_count = 0;
    struct limine_module_response *mods = module_req.response;
    for (uint64_t i = 0; mods && i < mods->module_count; i++) {
        if (out->module_count == BOOT_MAX_MODULES)
            panic("more than %d boot modules", BOOT_MAX_MODULES);
        struct limine_file *f = mods->modules[i];
        struct boot_module *m = &out->modules[out->module_count++];
        const char *base = f->path;
        for (const char *p = f->path; *p; p++)
            if (*p == '/')
                base = p + 1;
        size_t n = strlen(base);
        if (n >= sizeof m->name)
            n = sizeof m->name - 1;
        memcpy(m->name, base, n);
        m->name[n] = '\0';
        m->phys = (uint64_t)f->address - hh->offset;
        m->size = f->size;
    }

    if (mm->entry_count > BOOT_MAX_REGIONS)
        panic("memory map has %lu entries, max %d",
              (unsigned long)mm->entry_count, BOOT_MAX_REGIONS);
    out->region_count = mm->entry_count;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        out->regions[i] = (struct mem_region){
            .base = e->base, .length = e->length, .type = translate(e->type),
        };
    }
}