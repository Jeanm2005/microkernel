#include <arch/init.h>
#include "gdt.h"
#include "idt.h"

void arch_init_cpu(void)
{
    gdt_init();
    idt_init();
}