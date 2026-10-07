#pragma once
#include <stdbool.h>

/* Portable kernel self-tests (memory management). Same contract as
 * arch/selftest.h: the boot tests run every boot and must not crash; a
 * named test from `selftest=<name>` crashes on purpose or returns false
 * if the name is not one of ours. */
void core_selftest_boot(void);
bool core_selftest_run(const char *name);