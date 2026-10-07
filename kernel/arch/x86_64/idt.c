#include <stdint.h>
#include "gdt.h"
#include "idt.h"

struct idt_entry {
    uint16_t offset_lo;
    uint16_t selector;     /* code segment to run the handler in */
    uint8_t  ist;          /* IST slot (0 = don't switch stacks) */
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* type_attr: present | DPL | 64-bit interrupt gate (0xE).
 * An interrupt gate clears IF on entry, so handlers start with interrupts
 * off. DPL 3 lets ring-3 code raise the vector itself with `int N`. */
#define GATE_KERNEL 0x8e
#define GATE_USER   0xee

/* Defined in isr.S: address of each vector's entry stub. */
extern const uint64_t isr_stub_table[256];

static struct idt_entry idt[256] __attribute__((aligned(16)));

static void set_gate(int vec, uint64_t handler, uint8_t type_attr, uint8_t ist)
{
    idt[vec] = (struct idt_entry){
        .offset_lo = handler & 0xffff,
        .selector = GDT_KERNEL_CODE,
        .ist = ist,
        .type_attr = type_attr,
        .offset_mid = (handler >> 16) & 0xffff,
        .offset_hi = (uint32_t)(handler >> 32),
    };
}

void idt_init(void)
{
    for (int v = 0; v < 256; v++)
        set_gate(v, isr_stub_table[v], GATE_KERNEL, 0);

    set_gate(2,  isr_stub_table[2],  GATE_KERNEL, IST_NMI);
    set_gate(3,  isr_stub_table[3],  GATE_USER,   0);   /* int3 allowed from user mode */
    set_gate(8,  isr_stub_table[8],  GATE_KERNEL, IST_DOUBLE_FAULT);
    set_gate(18, isr_stub_table[18], GATE_KERNEL, IST_MACHINE_CHECK);

    struct idt_ptr ptr = { .limit = sizeof idt - 1, .base = (uint64_t)idt };
    __asm__ volatile("lidt %0" : : "m"(ptr) : "memory");
}