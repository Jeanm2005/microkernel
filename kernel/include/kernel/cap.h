/* Capability tables. Each process has one; a capability is named by its
 * slot index. A slot holds a reference to a kernel object plus the rights
 * and badge this particular capability carries. */
#pragma once
#include <stdint.h>

struct cspace;
struct kobj;
struct cap_node {
    struct kobj *obj;
    struct cap_node *parent, *next;
    uint64_t generation, parent_generation;
    uint32_t refs, users, depth;
};
struct cap_node *cap_node_new(struct kobj *obj, struct cap_node *parent);
void cap_node_get(struct cap_node *n);
void cap_node_put(struct cap_node *n);
int cap_node_valid(const struct cap_node *n);
void cap_use_get(struct cap_node *n);
void cap_use_put(struct cap_node *n);
int64_t cspace_insert_node(struct cspace *cs, struct cap_node *node, uint32_t rights,
                           uint64_t badge, int64_t dst);
int64_t cspace_revoke(struct cspace *cs, int64_t slot);


#define CSPACE_SLOTS 64

struct cap {
    struct cap_node *node;
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
/* Same, but into `slot` exactly. Returns the slot, -ERR_INVAL if out of
 * range, or -ERR_EXIST if it's in use. */
int64_t cspace_insert_at(struct cspace *cs, int64_t slot, struct kobj *obj,
                         uint32_t rights, uint64_t badge);

/* Find the capability in `slot`, check it's for an object of `type` (0 =
 * any) and has all of `rights`. Returns 0 and sets *out, or -ERR_BADCAP,
 * -ERR_TYPE or -ERR_PERM. */
int64_t cspace_lookup(struct cspace *cs, int64_t slot, uint32_t type, uint32_t rights,
                      struct cap **out);

/* Derive a new capability from `slot` with a subset of its rights. An
 * unbadged endpoint cap can be given a badge ("minted"); a badged one
 * can't be re-badged. Returns the new slot or an error. */
int64_t cspace_copy(struct cspace *cs, int64_t slot, uint32_t rights, uint64_t badge);
/* The same rules, from one table into another: into `dst_slot`, or the
 * lowest free slot if it is 0. */
int64_t cspace_derive(struct cspace *src, int64_t slot, struct cspace *dst, int64_t dst_slot,
                      uint32_t rights, uint64_t badge);

int64_t cspace_delete(struct cspace *cs, int64_t slot);

/* Delete every capability (the process is going away). */
void cspace_clear(struct cspace *cs);