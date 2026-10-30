/* System calls that let the root task read boot modules and build
 * processes out of them (SYS_MODULE_* and SYS_PROC_*). */
 #pragma once
 #include <stdint.h>

 struct cspace;

 int64_t sys_module_info(struct cspace *cs, int64_t mod, uint64_t name_buf, uint64_t buf_len);
 int64_t sys_module_read(struct cspace *cs, int64_t mod, uint64_t offset, uint64_t buf, uint64_t len);
 int64_t sys_proc_create(struct cspace *cs, int64_t factory, int64_t mod, uint64_t priority, uint64_t name, uint64_t name_len);
 int64_t sys_proc_give(struct cspace *cs, int64_t proc, int64_t slot, int64_t dst_slot, uint64_t rights, uint64_t badge);
 int64_t sys_proc_start(struct cspace *cs, int64_t proc, uint64_t arg);
 /* On success, *code is the exit code */
 int64_t sys_proc_wait(struct cspace *cs, int64_t proc, uint64_t *code);
int64_t sys_proc_watch(struct cspace *cs, int64_t proc, int64_t ntfn, uint64_t bits);
int64_t sys_proc_poll(struct cspace *cs, int64_t proc, uint64_t *code);

int64_t sys_proc_watchdog(struct cspace *cs, int64_t proc, uint64_t ticks);
