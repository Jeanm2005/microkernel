#pragma once

__attribute__((noreturn, format(printf, 3, 4)))
void panic_at(const char *file, int line, const char *fmt, ...);

#define kassert(cond)
    do {
        if (__builtin_expect(!(cond), 0))
            panic("assertion failed: %s", #cond);
    } while (0)