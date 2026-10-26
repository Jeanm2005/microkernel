/* User-visible memory: frames (physically contiguous RAM, paid for from a
 * factory's page budget) that a process maps into its own address space,
 * and splitting factories so each program gets a quota of its own. */
#pragma once
#include <stdint.h>

struct cspace;
struct process;

int64_t sys_frame_alloc(struct cspace *cs, int64_t factory, uint64_t pages);
/* Returns the physical address; *pages get the frame's size. */
int64_t sys_frame_info(struct cspace *cs, int64_t frame, uint64_t *pages);
int64_t sys_map(struct cspace *cs, int64_t frame, uint64_t vaddr, uint64_t writable);
int64_t sys_unmap(uint64_t vaddr);
int64_t sys_factory_split(struct cspace *cs, int64_t factory, uint64_t objects, uint64_t pages);

/* The process is going away (its address space is inactive): undo every
 * mapping, so the frames aren't freed with the process's own pages. */
void memory_unmap_all(struct process *p);