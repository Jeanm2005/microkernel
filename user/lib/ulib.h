/* Tiny user-space library: strings, printf over SYS_DEBUG_WRITE, and a
 * check() helper for self-testing programs. */
#pragma once
#include <stddef.h>

size_t strlen(const char *s);
void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);
int strcmp(const char *a, const char *b);

/* printf subset: %c %s %d %u %x %p %%, with an optional `l`. Output is
 * buffered and sent as one system call per printf. */
__attribute__((format(printf, 1, 2))) int printf(const char *fmt, ...);

/* Print "ok: what" or "FAILED: what"; failures counts the latter. */
extern int failures;
void check(int ok, const char *what);