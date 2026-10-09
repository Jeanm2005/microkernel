#include <arch/init.h>
#include <arch/user.h>
#include "gdt.h"
#include "idt.h"

void arch_init_cpu(void)
{
    gdt_init();
    idt_init();
    arch_syscall_init();
}