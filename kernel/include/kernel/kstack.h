/* Kernel stacks. Each thread gets KSTACK_SIZE bytes in a dedicated
 * virtual region, with an unmapped guard page range below it, so a stack
 * overflow faults immediately instead of silently corrupting whatever
 * memory lies below. */
 #pragma once
 #include <stdint.h>

 #define KSTACK_SIZE (16 * 1024)

 /* Returns the top of a new stack (stacks grow down), or 0 if out of
 * memory or stack slots. */
 uint64_t kstack_alloc(void);
 void kstack_free(uint64_t top);