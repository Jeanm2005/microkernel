/* The system-call ABI (application binary interface), shared by the
 * kernel and user programs. Nothing here may depend on kernel internals.
 *
 * Calling convention (x86_64): `syscall` instruction, number in rax,
 * arguments in rdi, rsi, rdx, r10, r8, r9, result in rax. The CPU uses rcx
 * and r11 to remember the return address and flags, so those two are
 * clobbered. Negative results are errors (-ERR_*). */
#pragma once

enum syscall_nr {
    SYS_DEBUG_WRITE = 0,   /* (const char *buf, size_t len) -> bytes written */
    SYS_EXIT        = 1,   /* (int code) -> does not return */
    SYS_YIELD       = 2,   /* () -> 0 */
    /* IPC and capability calls (M5) will follow from here. */
    SYS_COUNT
};

#define ERR_FAULT   14   /* a pointer argument isn't valid user memory */
#define ERR_INVAL   22   /* an argument is out of range */
#define ERR_NOSYS   38   /* no such system call */

/* Bytes one SYS_DEBUG_WRITE call will accept. */
#define DEBUG_WRITE_MAX 1024