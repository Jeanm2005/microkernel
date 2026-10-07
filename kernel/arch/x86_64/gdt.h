/* GDT (Global Descriptor Table) and TSS (Task State Segment).
 *
 * In 64-bit mode segmentation is mostly gone, but the CPU still needs:
 *  - code/data descriptors that say which privilege ring is running, and
 *  - a TSS that holds the stack pointers the CPU switches to on a trap.
 *
 * Selector order is fixed by the `syscall`/`sysret` instructions (M4):
 * sysret loads user SS from STAR+8 and user CS from STAR+16, so user data
 * must come directly before user code. */
#pragma once
#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18   /* used with RPL (requested privilege level) 3: 0x1b */
#define GDT_USER_CODE   0x20   /* used with RPL 3: 0x23 */
#define GDT_TSS         0x28   /* 16-byte descriptor, occupies two slots */

/* IST (Interrupt Stack Table) slots. An IDT entry that names an IST slot
 * always runs on that known-good stack, even if the current stack is
 * broken. That is what lets us report a double fault caused by a stack
 * overflow instead of triple-faulting (which resets the machine). */
#define IST_DOUBLE_FAULT  1
#define IST_NMI           2    /* NMI = non-maskable interrupt */
#define IST_MACHINE_CHECK 3

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];      /* stack to load when entering ring 0/1/2 from a less privileged ring */
    uint64_t reserved1;
    uint64_t ist[7];      /* ist[0] is IST slot 1 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;  /* offset of the I/O permission bitmap; >= limit means "none" */
} __attribute__((packed));

void gdt_init(void);

/* Kernel stack the CPU switches to when a ring-3 thread traps.
 * Set on every context switch once user mode exists (M4). */
void tss_set_kernel_stack(uint64_t rsp0);