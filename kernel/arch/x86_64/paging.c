/* x86_64 4-level paging. A virtual address splits into four 9-bit table
 * indexes plus a 12-bit page offset:
 *
 *   63..48  47..39  38..30  29..21  20..12  11..0
 *   sign    PML4    PDPT    PD      PT      offset
 *
 * PML4 = page map level 4, PDPT = page directory pointer table,
 * PD = page directory, PT = page table. A PD entry with the PS (page size)
 * bit maps a 2 MiB page directly instead of pointing to a PT. */
#include <arch/cpu.h>
#include <arch/paging.h>
#include <kernel/kprintf.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/string.h>

#define PTE_PRESENT  (1ull << 0)
#define PTE_WRITE    (1ull << 1)
#define PTE_USER     (1ull << 2)
#define PTE_PWT      (1ull << 3)    /* write-through */
#define PTE_PCD      (1ull << 4)    /* cache disable */
#define PTE_PS       (1ull << 7)    /* 2 MiB page (in a PD entry) */
#define PTE_GLOBAL   (1ull << 8)    /* survives CR3 reloads (kernel half only) */
#define PTE_NX       (1ull << 63)
#define PTE_ADDR     0x000ffffffffff000ull

#define HUGE_SIZE    ((uint64_t)2 << 20)
#define KERNEL_HALF  256            /* PML4 entries 256..511 are the kernel half */

typedef uint64_t pte_t;

static uint64_t kernel_root;
static pte_t nx_bit;                /* PTE_NX if the CPU supports it, else 0 */
static uint64_t table_frames;       /* page-table frames allocated, for stats */

/* Linker-script symbols (kernel/linker.ld). */
extern char __kernel_start[], __requests_start[], __requests_end[],
    __text_start[], __text_end[], __rodata_start[], __rodata_end[],
    __data_start[], __data_end[], __kernel_end[];

static pte_t *table(pte_t entry)
{
    return phys_to_virt(entry & PTE_ADDR);
}

static unsigned index_at(uint64_t virt, int level)   /* level 4 = PML4 ... 1 = PT */
{
    return (virt >> (12 + 9 * (level - 1))) & 0x1ff;
}

static uint64_t current_root(void)
{
    return read_cr3() & PTE_ADDR;
}

static pte_t leaf_bits(unsigned flags, uint64_t virt)
{
    pte_t e = PTE_PRESENT;
    if (flags & PAGE_WRITE)
        e |= PTE_WRITE;
    if (flags & PAGE_USER)
        e |= PTE_USER;
    if (!(flags & PAGE_EXEC))
        e |= nx_bit;
    if (flags & PAGE_UNCACHED)
        e |= PTE_PCD | PTE_PWT;
    if (index_at(virt, 4) >= KERNEL_HALF)
        e |= PTE_GLOBAL;
    return e;
}

/* Walk from the root down to the entry at `level` for `virt`, creating
 * missing tables if `create`. Returns NULL if a table is missing (and
 * !create), memory ran out, or a huge page is in the way. */
static pte_t *walk(uint64_t root, uint64_t virt, int level, bool create)
{
    pte_t *t = phys_to_virt(root);
    for (int l = 4; l > level; l--) {
        pte_t *e = &t[index_at(virt, l)];
        if (!(*e & PTE_PRESENT)) {
            if (!create)
                return NULL;
            uint64_t frame = pmm_alloc_zeroed();
            if (frame == PMM_NONE)
                return NULL;
            table_frames++;
            /* Intermediate entries allow everything; the leaf entry decides
             * the real permissions. (x86 needs USER at every level for a
             * user page to be reachable.) */
            *e = frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
        } else if (*e & PTE_PS) {
            return NULL;
        }
        t = table(*e);
    }
    return &t[index_at(virt, level)];
}

static void flush(uint64_t root, uint64_t virt)
{
    /* invlpg also drops global entries, so this is enough even for the
     * kernel half, as long as only one CPU is running. */
    if (root == current_root() || index_at(virt, 4) >= KERNEL_HALF)
        invlpg(virt);
}

bool paging_translate(uint64_t root, uint64_t virt, uint64_t *phys_out)
{
    pte_t *t = phys_to_virt(root);
    for (int l = 4; l >= 1; l--) {
        pte_t e = t[index_at(virt, l)];
        if (!(e & PTE_PRESENT))
            return false;
        if (l == 1 || (l == 2 && (e & PTE_PS))) {
            uint64_t page = (l == 2) ? HUGE_SIZE : PAGE_SIZE;
            uint64_t base = (l == 2) ? (e & PTE_ADDR & ~(HUGE_SIZE - 1)) : (e & PTE_ADDR);
            if (phys_out)
                *phys_out = base + (virt & (page - 1));
            return true;
        }
        t = table(e);
    }
    return false;
}

bool paging_map(uint64_t root, uint64_t virt, uint64_t phys, unsigned flags)
{
    kassert(IS_ALIGNED(virt, PAGE_SIZE) && IS_ALIGNED(phys, PAGE_SIZE));
    pte_t *e = walk(root, virt, 1, true);
    if (!e || (*e & PTE_PRESENT))
        return false;
    *e = phys | leaf_bits(flags, virt);
    flush(root, virt);
    return true;
}

static bool map_huge(uint64_t root, uint64_t virt, uint64_t phys, unsigned flags)
{
    pte_t *e = walk(root, virt, 2, true);
    if (!e || (*e & PTE_PRESENT))
        return false;
    *e = phys | leaf_bits(flags, virt) | PTE_PS;
    flush(root, virt);
    return true;
}

bool paging_map_range(uint64_t root, uint64_t virt, uint64_t phys,
                      uint64_t len, unsigned flags)
{
    kassert(IS_ALIGNED(virt, PAGE_SIZE) && IS_ALIGNED(phys, PAGE_SIZE));
    uint64_t end = virt + ALIGN_UP(len, PAGE_SIZE);
    while (virt < end) {
        uint64_t existing;
        if (paging_translate(root, virt, &existing)) {
            if (existing != phys)
                return false;            /* mapped to something else */
            virt += PAGE_SIZE;           /* same mapping already there */
            phys += PAGE_SIZE;
            continue;
        }
        bool huge_ok = IS_ALIGNED(virt, HUGE_SIZE) && IS_ALIGNED(phys, HUGE_SIZE) &&
                       end - virt >= HUGE_SIZE;
        if (huge_ok && map_huge(root, virt, phys, flags)) {
            virt += HUGE_SIZE;
            phys += HUGE_SIZE;
            continue;
        }
        if (!paging_map(root, virt, phys, flags))
            return false;
        virt += PAGE_SIZE;
        phys += PAGE_SIZE;
    }
    return true;
}

uint64_t paging_unmap(uint64_t root, uint64_t virt)
{
    kassert(IS_ALIGNED(virt, PAGE_SIZE));
    pte_t *e = walk(root, virt, 1, false);
    if (!e || !(*e & PTE_PRESENT))
        return 0;
    uint64_t phys = *e & PTE_ADDR;
    *e = 0;
    flush(root, virt);
    return phys;
}

uint64_t paging_kernel_root(void) { return kernel_root; }

uint64_t paging_new_root(void)
{
    uint64_t root = pmm_alloc_zeroed();
    if (root == PMM_NONE)
        return PMM_NONE;
    table_frames++;
    /* Share the kernel half by copying its 256 PML4 entries. This stays
     * correct forever because paging_init() created all 256 PDPTs up front,
     * so the kernel's PML4 entries never change after boot. */
    pte_t *dst = phys_to_virt(root), *src = phys_to_virt(kernel_root);
    for (int i = KERNEL_HALF; i < 512; i++)
        dst[i] = src[i];
    return root;
}

static void free_tables(pte_t entry, int level, bool free_frames)
{
    /* `entry` points to a table at `level`; free everything below it. */
    pte_t *t = table(entry);
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & PTE_PRESENT))
            continue;
        if (level == 1 || (t[i] & PTE_PS)) {
            /* A leaf: a mapped page. 2 MiB user pages aren't used yet. */
            kassert(level == 1);
            if (free_frames)
                pmm_free(t[i] & PTE_ADDR);
        } else {
            free_tables(t[i], level - 1, free_frames);
        }
    }
    pmm_free(entry & PTE_ADDR);
    table_frames--;
}

void paging_destroy_root(uint64_t root, bool free_frames)
{
    kassert(root != kernel_root && root != current_root());
    pte_t *t = phys_to_virt(root);
    for (int i = 0; i < KERNEL_HALF; i++)   /* user half only */
        if (t[i] & PTE_PRESENT)
            free_tables(t[i], 3, free_frames);
    pmm_free(root);
    table_frames--;
}

bool paging_user_range_ok(uint64_t root, uint64_t virt, uint64_t len, bool write)
{
    if (len == 0)
        return true;
    uint64_t end = virt + len;
    if (end < virt || end > (1ull << 47))   /* overflow, or reaches the kernel half */
        return false;

    pte_t need = PTE_PRESENT | PTE_USER | (write ? PTE_WRITE : 0);
    for (uint64_t page = ALIGN_DOWN(virt, PAGE_SIZE); page < end; page += PAGE_SIZE) {
        pte_t *t = phys_to_virt(root);
        for (int l = 4; l >= 1; l--) {
            pte_t e = t[index_at(page, l)];
            if ((e & need) != need)
                return false;
            if (l == 1 || (e & PTE_PS))
                break;
            t = table(e);
        }
    }
    return true;
}

void paging_activate(uint64_t root)
{
    write_cr3(root);
}

static void map_or_panic(uint64_t virt, uint64_t phys, uint64_t len, unsigned flags,
                         const char *what)
{
    if (!paging_map_range(kernel_root, virt, phys, len, flags))
        panic("paging: failed to map %s (%p -> %p, %lu bytes)",
              what, (void *)virt, (void *)phys, len);
}

static void map_kernel_section(const struct boot_info *boot, char *start, char *end,
                               unsigned flags, const char *what)
{
    uint64_t virt = (uint64_t)start;
    uint64_t phys = virt - boot->kernel_virt_base + boot->kernel_phys_base;
    map_or_panic(virt, phys, (uint64_t)(end - start), flags, what);
}

static void enable_cpu_features(void)
{
    uint32_t a, b, c, d;
    cpuid(0x80000001, &a, &b, &c, &d);
    if (d & (1u << 20)) {            /* NX supported */
        wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
        nx_bit = PTE_NX;
    } else {
        kprintf("paging: CPU has no NX bit; data pages will be executable\n");
    }
    write_cr4(read_cr4() | CR4_PGE);
}

void paging_init(const struct boot_info *boot)
{
    enable_cpu_features();

    kernel_root = pmm_alloc_zeroed();
    if (kernel_root == PMM_NONE)
        panic("paging: out of memory");
    table_frames++;

    /* Create every kernel-half PDPT now (256 frames = 1 MiB), so the
     * kernel's PML4 entries never change and new address spaces can just
     * copy them. */
    pte_t *pml4 = phys_to_virt(kernel_root);
    for (int i = KERNEL_HALF; i < 512; i++) {
        uint64_t f = pmm_alloc_zeroed();
        if (f == PMM_NONE)
            panic("paging: out of memory");
        table_frames++;
        pml4[i] = f | PTE_PRESENT | PTE_WRITE;   /* no USER: kernel only */
    }

    /* HHDM: all RAM-like regions, read/write, never executable. Device
     * memory (APIC etc.) is mapped by the drivers that need it. */
    for (size_t i = 0; i < boot->region_count; i++) {
        const struct mem_region *r = &boot->regions[i];
        switch (r->type) {
        case MEM_USABLE:
        case MEM_BOOTLOADER_RECLAIMABLE:
        case MEM_KERNEL_AND_MODULES:
        case MEM_ACPI_RECLAIMABLE:
        case MEM_ACPI_NVS: {
            uint64_t base = ALIGN_DOWN(r->base, PAGE_SIZE);
            uint64_t end = ALIGN_UP(r->base + r->length, PAGE_SIZE);
            map_or_panic(hhdm_offset + base, base, end - base, PAGE_WRITE, "HHDM");
            break;
        }
        default:
            break;
        }
    }

    /* Kernel image, one section at a time: W^X (write xor execute). */
    map_kernel_section(boot, __requests_start, __requests_end, PAGE_WRITE, ".limine_requests");
    map_kernel_section(boot, __text_start, __text_end, PAGE_EXEC, ".text");
    map_kernel_section(boot, __rodata_start, __rodata_end, 0, ".rodata");
    map_kernel_section(boot, __data_start, __data_end, PAGE_WRITE, ".data/.bss");

    paging_activate(kernel_root);
    kprintf("paging: kernel page tables active (cr3 %p, %lu table frames, NX %s)\n",
            (void *)kernel_root, table_frames, nx_bit ? "on" : "off");
}