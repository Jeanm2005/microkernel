#include <stdint.h>
#include <arch/selftest.h>
#include <kernel/kprintf.h>
#include <kernel/panic.h>
#include <kernel/string.h>

void arch_selftest_boot(void)
{
    static volatile int resumed;
    resumed = 0;
    __asm__ volatile("int3");
    resumed = 1;
    kassert(resumed);
    kprintf("selftest: breakpoint handled and resumed\n");
}

/* Write to an address nothing maps. Expect a #PF report naming it. */
static void test_page_fault(void)
{
    kprintf("selftest: writing to unmapped address 0xdeadb000\n");
    *(volatile uint64_t *)0xdeadb000 = 1;
}

/* Point RSP at unmapped memory and push. The push faults (#PF), and the CPU
 * can't push the #PF frame onto the same broken stack, so it raises a
 * double fault. The #DF gate uses an IST stack, so we get a report instead
 * of a triple fault. This checks that the TSS and IST setup really work. */
static void test_double_fault(void)
{
    kprintf("selftest: switching to an unmapped stack\n");
    __asm__ volatile(
        "movabs $0x00007fff00000000, %%rsp\n\t"
        "pushq $0\n\t"
        ::: "memory");
}

/* Write to the kernel's own code. .text is mapped read-only + executable,
 * so this must fault: proves W^X (write xor execute) is enforced. */
static void test_write_text(void)
{
    volatile uint8_t *code = (volatile uint8_t *)(uintptr_t)&arch_selftest_boot;
    kprintf("selftest: writing to kernel code at %p\n", (void *)code);
    *code = 0xc3;
}

/* Jump into a writable data page holding a `ret` instruction. Data pages
 * are mapped NX (no-execute), so the instruction fetch must fault. */
static void test_exec_data(void)
{
    static uint8_t code[16] = { 0xc3 };   /* ret */
    kprintf("selftest: calling into data at %p\n", (void *)code);
    ((void (*)(void))(uintptr_t)code)();
}

bool arch_selftest_run(const char *name)
{
    if (strcmp(name, "pagefault") == 0)
        test_page_fault();
    else if (strcmp(name, "doublefault") == 0)
        test_double_fault();
    else if (strcmp(name, "write-text") == 0)
        test_write_text();
    else if (strcmp(name, "exec-data") == 0)
        test_exec_data();
    else
        return false;
    panic("selftest '%s' returned; it should have crashed", name);
}