#pragma once
#include <stdarg.g>

__attribute__((format(printf, 1, 2))) void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list ap);