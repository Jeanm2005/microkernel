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