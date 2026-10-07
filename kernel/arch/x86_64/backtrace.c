#include <arch/backtrace.h>
#include <kernel/kprintf.h>

#define MAX_FRAMES 16

/* With frame pointers, every function starts with
 *     push %rbp ; mov %rsp, %rbp
 * so [rbp] holds the caller's rbp and [rbp + 8] the return address. */
struct frame {
    struct frame *next;
    uint64_t return_addr;
};

static int plausible(const struct frame *f)
{
    uint64_t a = (uint64_t)f;
    return a >= 0xffff800000000000ull && (a & 7) == 0;   /* higher half, aligned */
}

void backtrace_print(uint64_t frame_ptr)
{
    const struct frame *f = (const struct frame *)frame_ptr;
    for (int i = 0; i < MAX_FRAMES && plausible(f) && f->return_addr; i++) {
        kprintf("    %p\n", (void *)f->return_addr);
        f = f->next;
    }
}