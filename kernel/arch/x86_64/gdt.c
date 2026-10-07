#include <stddef.h>
#include "gdt.h"

/* Access byte bits */
#define A_PRESENT  0x80
#define A_DPL3     0x60   /* DPL = descriptor privilege level */
#define A_CODEDATA 0x10   /* 1 = code/data segment, 0 = system (e.g. TSS) */
#define A_EXEC     0x08
#define A_RW       0x02   /* readable code / writable data */
#define A_TSS64    0x09   /* system type: available 64-bit TSS */

/* Flags nibble (upper 4 bits of byte 6) */
#define F_LONG     0x2    /* 64-bit code segment */
#define F_GRAN4K   0x8

struct gdt_entry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  limit_hi_flags;
    uint8_t  base_hi;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define SEG(acc, flg) {                                   \
    .limit_lo = 0xffff, .access = (acc),                  \
    .limit_hi_flags = (uint8_t)(((flg) << 4) | 0x0f) }

/* Base and limit are ignored in 64-bit mode for code/data; we set limit to
 * the maximum by convention. Slots 5-6 are filled with the TSS at init. */
static struct gdt_entry gdt[7] __attribute__((aligned(16))) = {
    [0] = {0},
    [1] = SEG(A_PRESENT | A_CODEDATA | A_EXEC | A_RW,          F_LONG | F_GRAN4K),
    [2] = SEG(A_PRESENT | A_CODEDATA | A_RW,                   F_GRAN4K),
    [3] = SEG(A_PRESENT | A_DPL3 | A_CODEDATA | A_RW,          F_GRAN4K),
    [4] = SEG(A_PRESENT | A_DPL3 | A_CODEDATA | A_EXEC | A_RW, F_LONG | F_GRAN4K),
};

static struct tss tss;

#define IST_STACK_SIZE 16384
static uint8_t ist_stacks[3][IST_STACK_SIZE] __attribute__((aligned(16)));

static void install_tss(void)
{
    uint64_t base = (uint64_t)&tss;
    uint32_t limit = sizeof tss - 1;

    gdt[5] = (struct gdt_entry){
        .limit_lo = limit & 0xffff,
        .base_lo = base & 0xffff,
        .base_mid = (base >> 16) & 0xff,
        .access = A_PRESENT | A_TSS64,
        .limit_hi_flags = (limit >> 16) & 0x0f,
        .base_hi = (base >> 24) & 0xff,
    };
    /* A 64-bit TSS descriptor is 16 bytes: the second half holds base[63:32]. */
    uint32_t *hi = (uint32_t *)&gdt[6];
    hi[0] = (uint32_t)(base >> 32);
    hi[1] = 0;
}

void gdt_init(void)
{
    /* Stacks grow down, so each IST entry points at the top of its stack. */
    tss.ist[IST_DOUBLE_FAULT - 1]  = (uint64_t)&ist_stacks[0][IST_STACK_SIZE];
    tss.ist[IST_NMI - 1]           = (uint64_t)&ist_stacks[1][IST_STACK_SIZE];
    tss.ist[IST_MACHINE_CHECK - 1] = (uint64_t)&ist_stacks[2][IST_STACK_SIZE];
    tss.iomap_base = sizeof tss;   /* no I/O bitmap yet (I/O-port caps come later) */
    install_tss();

    struct gdt_ptr ptr = { .limit = sizeof gdt - 1, .base = (uint64_t)gdt };

    /* Load the new GDT, then reload every segment register so none of them
     * still refers to Limine's GDT. CS can't be loaded with `mov`, so we
     * use a far return: push the new CS and a return address, then lretq. */
    __asm__ volatile(
        "lgdt %0\n\t"
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "mov %2, %%ds\n\t"
        "mov %2, %%es\n\t"
        "mov %2, %%ss\n\t"
        "mov %3, %%fs\n\t"
        "mov %3, %%gs\n\t"
        :
        : "m"(ptr), "i"(GDT_KERNEL_CODE), "r"((uint16_t)GDT_KERNEL_DATA),
          "r"((uint16_t)0)
        : "rax", "memory");

    /* Tell the CPU where the TSS is. */
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS) : "memory");
}

void tss_set_kernel_stack(uint64_t rsp0)
{
    tss.rsp[0] = rsp0;
}