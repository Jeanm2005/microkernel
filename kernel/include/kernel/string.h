#pragma once
#include <stddef.h>

/* GCC may emit calls to these even with -ffreestanding, so they must exist. */
void *memcpy(void *restrict dst, const void *restrict src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

/* Not used by GCC, but handy. */
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);