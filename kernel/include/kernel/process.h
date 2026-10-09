/* User processes. For now a process is one address space plus one thread;
 * capability tables join it in M5. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* User stack: 16 KiB just below the top of the user half, with one
 * unmapped page above it and nothing mapped below it, so running off
 * either end faults. */
#define USER_STACK_TOP   (0x0000800000000000ull - 0x1000)
#define USER_STACK_SIZE  (16 * 1024)

struct thread;

struct process {
    char           name[24];
    uint64_t       root;           /* page-table root (address space) */
    struct thread *thread;
    uint64_t       entry;          /* first user instruction */
    uint64_t       arg;            /* passed to the program in rdi */

    /* Status, written once by the dying thread and read by waiters. */
    volatile bool  exited;
    int            exit_code;      /* valid if exited && !killed */
    bool           killed;         /* stopped by the kernel after a fault */
    const char    *kill_reason;

    uint64_t       run_ticks;      /* timer ticks its thread ran for, set at exit */
    int            refs;           /* the thread + whoever holds the pointer */
    bool           at_line_start;  /* for prefixing debug output with the name */
};

/* Load an ELF image into a new address space and start it as a thread at
 * `priority`, with `arg` as main()'s argument. Returns NULL on failure
 * (reason printed). The caller gets a reference: release it with
 * process_put(). */
struct process *process_spawn(const char *name, const void *elf, size_t size,
                              uint64_t arg, uint8_t priority);

/* Sleep until the process has exited or been killed. */
void process_wait(struct process *p);
void process_put(struct process *p);

/* For the current user thread: end the process normally (system call) or
 * forcibly (fatal fault). Neither returns. */
__attribute__((noreturn)) void process_exit_current(int code);
__attribute__((noreturn)) void process_kill_current(const char *reason);

/* Called when the process's thread has been reaped and its address space
 * is no longer active. */
void process_thread_gone(struct process *p);

/* SYS_DEBUG_WRITE: copy `len` bytes from user memory and print them,
 * each line prefixed with the process name. */
int64_t process_debug_write(uint64_t user_buf, uint64_t len);