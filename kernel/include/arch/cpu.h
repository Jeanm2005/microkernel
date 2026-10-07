/* x86_64 CPU primitives. Everything architecture-specific that core code
 * needs goes through headers in include/arch/ so a port only touches arch/. */
#pragma once
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline void cpu_cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void cpu_relax(void) { __asm__ volatile("pause"); }

/* Stop this CPU for good: interrupts off, then hlt forever. */
__attribute__((noreturn)) static inline void cpu_halt_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* Control registers. CR0 = mode flags (paging, write-protect),
 * CR2 = faulting address after a page fault, CR3 = physical address of the
 * top-level page table, CR4 = feature enables. */
#define DEFINE_READ_CR(n)                                       \
    static inline uint64_t read_cr##n(void)                     \
    {                                                           \
        uint64_t v;                                             \
        __asm__ volatile("mov %%cr" #n ", %0" : "=r"(v));       \
        return v;                                               \
    }
DEFINE_READ_CR(0)
DEFINE_READ_CR(2)
DEFINE_READ_CR(3)
DEFINE_READ_CR(4)
#undef DEFINE_READ_CR

static inline void write_cr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline void write_cr4(uint64_t v) { __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory"); }

#define CR4_PGE (1ull << 7)   /* page global enable: keep G-flagged TLB entries across CR3 loads */

/* Drop the TLB (translation lookaside buffer) entry for one virtual page. */
static inline void invlpg(uint64_t virt)
{
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

/* MSRs (model-specific registers) hold CPU configuration such as EFER. */
#define MSR_EFER  0xc0000080
#define EFER_NXE  (1ull << 11)  /* allow the NX (no-execute) page-table bit */

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* CPUID reports which features the processor supports. */
static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}