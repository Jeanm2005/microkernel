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

bool arch_selftest_run(const char *name)
{
    if (strcmp(name, "pagefault") == 0)
        test_page_fault();
    else if (strcmp(name, "doublefault") == 0)
        test_double_fault();
    else
        return false;
    panic("selftest '%s' returned; it should have crashed", name);
}