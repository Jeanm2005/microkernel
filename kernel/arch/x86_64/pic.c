#include <arch/cpu.h>
#include <arch/irq.h>
#include "lapic.h"
#include "pic.h"

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xa0
#define PIC2_DATA 0xa1
#define PIC_EOI 0x20
#define OCW3_ISR 0x0b /* next read of the command port returns the in-service register */

static uint16_t mask = 0xffff; /* bit set = line masked; bit 2 is the cascade */

static void write_mask(void)
{
	/* The slave PIC reaches the CPU through master line 2, so line 2 must
	 * be open whenever any slave line is */
	uint16_t m = mask;
	if ((m & 0xff00) != 0xff00)
		m &= ~(1u << 2);
	outb(PIC1_DATA, m & 0xff);
	outb(PIC2_DATA, m >> 8);
}

void pic_init(void)
{
	outb(PIC1_CMD, 0x11); outb(PIC2_CMD, 0x11);
	outb(PIC1_DATA, VEC_PIC_BASE); outb(PIC2_DATA, VEC_PIC_BASE + 8);
	outb(PIC1_DATA, 0x04); outb(PIC2_DATA, 0x02);
	outb(PIC1_DATA, 0x01); outb(PIC2_DATA, 0x01);
	mask = 0xffff;
	write_mask();
}

void arch_irq_mask(unsigned line)
{
	uint64_t flags = irq_save();
	mask |= 1u << line;
	write_mask();
	irq_restore(flags);
}

void arch_irq_unmask(unsigned line)
{
	uint64_t flags = irq_save();
	mask &= ~(1u << line);
	write_mask();
	irq_restore(flags);
}

static uint8_t in_service(uint16_t cmd_port)
{
	outb(cmd_port, OCW3_ISR);
	return inb(cmd_port);
}

bool pic_accept(unsigned line)
{
	if (line == 7 && !(in_service(PIC1_CMD) & 0x80))
		return false;
	if (line == 15 && !(in_service(PIC2_CMD) & 0x80)) {
		outb(PIC1_CMD, PIC_EOI);
		return false;
	}
	arch_irq_mask(line);
	if (line >= 8)
		outb(PIC2_CMD, PIC_EOI);
	outb(PIC1_CMD, PIC_EOI);
	return true;
}
