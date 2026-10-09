/* Minimal ELF (Executable and Linkable Format) loader for static user
 * programs. The image comes from a boot module, but it is still treated as
 * untrusted input: every offset and size is bounds-checked before use. */
#include <stdbool.h>
#include <arch/paging.h>
#include <arch/user.h>
#include <kernel/elf.h>
#include <kernel/mm.h>
#include <kernel/pmm.h>
#include <kernel/string.h>

/* The parts of the ELF64 format we use. */
struct elf64_ehdr {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

#define ET_EXEC     2
#define EM_X86_64   62
#define PT_LOAD     1
#define PF_X        1
#define PF_W        2

enum {
    E_SHORT = -1, E_MAGIC = -2, E_ARCH = -3, E_TYPE = -4, E_PHDRS = -5,
    E_SEGMENT = -6, E_RANGE = -7, E_WX = -8, E_OVERLAP = -9, E_NOMEM = -10,
    E_ENTRY = -11,
};

const char *elf_strerror(int err)
{
    switch (err) {
    case E_SHORT:   return "file too short";
    case E_MAGIC:   return "not an ELF file";
    case E_ARCH:    return "not a 64-bit little-endian x86_64 ELF";
    case E_TYPE:    return "not a static executable (ET_EXEC)";
    case E_PHDRS:   return "program headers out of bounds";
    case E_SEGMENT: return "segment data out of bounds";
    case E_RANGE:   return "segment outside the user address range";
    case E_WX:      return "segment is both writable and executable";
    case E_OVERLAP: return "segments overlap";
    case E_NOMEM:   return "out of memory";
    case E_ENTRY:   return "entry point is not in an executable segment";
    default:        return "unknown error";
    }
}

/* a + b <= limit, without overflow. */
static bool fits(uint64_t a, uint64_t b, uint64_t limit)
{
    return a <= limit && b <= limit - a;
}

static int load_segment(uint64_t root, const uint8_t *file, const struct elf64_phdr *ph)
{
    unsigned flags = PAGE_USER;
    if (ph->flags & PF_W)
        flags |= PAGE_WRITE;
    if (ph->flags & PF_X)
        flags |= PAGE_EXEC;

    uint64_t start = ALIGN_DOWN(ph->vaddr, PAGE_SIZE);
    uint64_t end = ALIGN_UP(ph->vaddr + ph->memsz, PAGE_SIZE);
    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        if (paging_translate(root, page, NULL))
            return E_OVERLAP;   /* each page belongs to exactly one segment */
        uint64_t frame = pmm_alloc_zeroed();
        if (frame == PMM_NONE)
            return E_NOMEM;
        if (!paging_map(root, page, frame, flags)) {
            pmm_free(frame);
            return E_NOMEM;
        }

        /* Copy the part of the file data that falls into this page. The
         * rest (alignment padding, .bss) stays zero. */
        uint64_t data_start = ph->vaddr, data_end = ph->vaddr + ph->filesz;
        uint64_t lo = page > data_start ? page : data_start;
        uint64_t hi = page + PAGE_SIZE < data_end ? page + PAGE_SIZE : data_end;
        if (lo < hi)
            memcpy((uint8_t *)phys_to_virt(frame) + (lo - page),
                   file + ph->offset + (lo - data_start), hi - lo);
    }
    return 0;
}

int elf_load(uint64_t root, const void *image, size_t size, uint64_t *entry)
{
    const uint8_t *file = image;
    if (size < sizeof(struct elf64_ehdr))
        return E_SHORT;
    const struct elf64_ehdr *eh = image;
    if (memcmp(eh->ident, "\x7f" "ELF", 4) != 0)
        return E_MAGIC;
    if (eh->ident[4] != 2 || eh->ident[5] != 1 || eh->machine != EM_X86_64)
        return E_ARCH;
    if (eh->type != ET_EXEC)
        return E_TYPE;
    if (eh->phentsize != sizeof(struct elf64_phdr) ||
        !fits(eh->phoff, (uint64_t)eh->phnum * sizeof(struct elf64_phdr), size))
        return E_PHDRS;

    const struct elf64_phdr *ph = (const struct elf64_phdr *)(file + eh->phoff);
    bool entry_ok = false;
    for (unsigned i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *p = &ph[i];
        if (p->type != PT_LOAD || p->memsz == 0)
            continue;
        if (p->filesz > p->memsz || !fits(p->offset, p->filesz, size))
            return E_SEGMENT;
        if (p->vaddr < USER_BASE || !fits(p->vaddr, p->memsz, USER_TOP))
            return E_RANGE;
        if ((p->flags & PF_W) && (p->flags & PF_X))
            return E_WX;
        int err = load_segment(root, file, p);
        if (err)
            return err;
        if ((p->flags & PF_X) && eh->entry >= p->vaddr && eh->entry < p->vaddr + p->memsz)
            entry_ok = true;
    }
    if (!entry_ok)
        return E_ENTRY;
    *entry = eh->entry;
    return 0;
}