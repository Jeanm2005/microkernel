/* System-call wrappers for user programs. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <abi/syscall.h>

static inline long syscall2(long nr, long a0, long a1)
{
    long ret;
    /* `syscall` overwrites rcx (return address) and r11 (flags). */
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline long sys_debug_write(const void *buf, size_t len)
{
    return syscall2(SYS_DEBUG_WRITE, (long)buf, (long)len);
}

__attribute__((noreturn)) static inline void sys_exit(int code)
{
    syscall2(SYS_EXIT, code, 0);
    __builtin_unreachable();
}

static inline long sys_yield(void)
{
    return syscall2(SYS_YIELD, 0, 0);
}