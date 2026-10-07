#pragma once
#include <stdbool.h>

/* CPU-level tests; portable memory tests are in kernel/selftest.h.
 *
 * Non-destructive check run on every boot: raises a breakpoint exception
 * (#BP) and verifies the handler returns to the next instruction. */
void arch_selftest_boot(void);

/* Destructive tests chosen with `selftest=<name>` on the kernel command
 * line. Each one deliberately crashes the kernel in a specific way so
 * scripts/test.sh can check the crash report. Returns false if `name` is
 * unknown; otherwise does not return. */
bool arch_selftest_run(const char *name);