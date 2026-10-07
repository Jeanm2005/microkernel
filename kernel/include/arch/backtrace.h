#pragma once
#include <stdint.h>

/* Print return addresses by walking the frame-pointer chain starting at
 * `frame_ptr` (an RBP value). Turn addresses into source lines with:
 *     addr2line -e build/kernel.elf -f -p <addr> ...
 * Works because the kernel is built with -fno-omit-frame-pointer. */
void backtrace_print(uint64_t frame_ptr);