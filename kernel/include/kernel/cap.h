/* Capability tables. Each process has one; a capability is named by its
 * slot index. A slot holds a reference to a kernel object plus the rights
 * and badge this particular capability carries. */
#pragma once
#include <stdint.h>

struct kobj;

#define CSPACE_SLOTS 64

struct cap {
    struct kobj *obj;      /* NULL = empty slot */
    uint32_t     rights;   /* CAP_READ | CAP_WRITE | CAP_GRANT */
    uint64_t     badge;    /* stamped on messages sent through this cap */
};

struct cspace {
    struct cap slot[CSPACE_SLOTS];   /* slot 0 is never used */
};

/* Put a new capability in the lowest free slot; takes its own reference
 * to `obj`. Returns the slot, or -ERR_FULL. */
int64_t cspace_insert(struct cspace *cs, struct kobj *obj, uint32_t rights, uint64_t badge);

/* Find the capability in `slot`, check it's for an object of `type` (0 =
 * any) and has all of `rights`. Returns 0 and sets *out, or -ERR_BADCAP,
 * -ERR_TYPE or -ERR_PERM. */
int64_t cspace_lookup(struct cspace *cs, int64_t slot, uint32_t type, uint32_t rights,
                      struct cap **out);

/* Derive a new capability from `slot` with a subset of its rights. An
 * unbadged endpoint cap can be given a badge ("minted"); a badged one
 * can't be re-badged. Returns the new slot or an error. */
int64_t cspace_copy(struct cspace *cs, int64_t slot, uint32_t rights, uint64_t badge);

int64_t cspace_delete(struct cspace *cs, int64_t slot);

/* Delete every capability (the process is going away). */
void cspace_clear(struct cspace *cs);