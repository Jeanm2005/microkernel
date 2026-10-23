/* Tiny user-space library: strings, formatted output, and a check()
 * helper for self-testing programs. */
#pragma once
#include <stdarg.h>
#include <stddef.h>

size_t strlen(const char *s);
void *memset(void *dst, int c, size_t n);
void *memcpy(void *restrict dst, const void *restrict src, size_t n);
int strcmp(const char *a, const char *b);

/* Format subset: %c %s %d %u %x %p %%, with an optional `l`. Output that
 * doesn't fit is cut off; the result is always NUL-terminated. Returns
 * the length written. */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
__attribute__((format(printf, 3, 4)))
int snprintf(char *buf, size_t size, const char *fmt, ...);

/* printf sends each call's output (up to 512 bytes) as one unit: through
 * the console server once printf_use_console() has named its endpoint,
 * otherwise through SYS_DEBUG_WRITE (which only processes the kernel
 * started itself may use). */
__attribute__((format(printf, 1, 2))) int printf(const char *fmt, ...);
void printf_use_console(long ep);   /* 0 = back to SYS_DEBUG_WRITE */

/* Print "ok: what" or "FAILED: what"; failures counts the latter. */
extern int failures;
void check(int ok, const char *what);
