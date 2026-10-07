#include <arch/cpu.h>
#include <arch/qemu.h>

void qemu_exit(uint32_t code)
{
    outl(0xf4, code);
}