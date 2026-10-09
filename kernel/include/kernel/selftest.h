#pragma once
#include <stdbool.h>
#include <kernel/boot.h>

/* Portable kernel self-tests (memory management). Same contract as
 * arch/selftest.h: the boot tests run every boot and must not crash; a
 * named test from `selftest=<name>` crashes on purpose or returns false
 * if the name is not one of ours. */
void core_selftest_boot(void);
bool core_selftest_run(const char *name);

/* Scheduler tests. These need running threads, so they are called from
 * the init thread instead of from kmain(). */
void sched_selftest_boot(void);
bool sched_selftest_run(const char *name);

/* User-mode tests: run the root task from the boot modules. The boot test
 * checks a clean run; the named ones (user-pagefault, user-privileged,
 * user-kernel-read) make it misbehave and check that only it is killed.
 * Unlike the others, those named tests return normally. */
void user_selftest_boot(const struct boot_info *boot);
bool user_selftest_run(const char *name, const struct boot_info *boot);

/* IPC tests: a server and a client process built from the root task
 * image, connected by an endpoint the kernel sets up. The named test
 * ipc-server-dies crashes the server mid-conversation and checks the
 * client gets -ERR_DEAD instead of hanging; it returns normally. */
void ipc_selftest_boot(const struct boot_info *boot);
bool ipc_selftest_run(const char *name, const struct boot_info *boot);