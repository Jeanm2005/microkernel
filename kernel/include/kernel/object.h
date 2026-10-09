/* Kernel objects that user programs reach through capabilities. Each is
 * reference-counted: every capability to it holds a reference, and so does
 * every thread blocked on it. It is freed when the last one goes. */
#pragma once
#include <stdint.h>
#include <kernel/sched.h>

enum kobj_type {
    KOBJ_ENDPOINT = 1,       /* same values as OBJ_* in abi/syscall.h */
    KOBJ_NOTIFICATION = 2,
    KOBJ_FACTORY = 3,
};

struct factory;

struct kobj {
    uint32_t        type;
    uint32_t        refs;
    struct factory *origin;   /* refunded when this object is freed, or NULL */
};

/* Synchronous rendezvous point. At most one of the two queues is
 * non-empty at any time. */
struct endpoint {
    struct kobj  obj;
    struct waitq senders;     /* threads blocked in send/call */
    struct waitq receivers;   /* threads blocked in recv */
};

struct notification {
    struct kobj  obj;
    uint64_t     word;        /* pending bits */
    struct waitq waiters;
};

/* The right to create kernel objects, with a budget. This replaces any
 * ambient "anyone may allocate" authority: a process can only make kernel
 * objects (and so consume kernel memory) if it holds a factory capability,
 * and only as many as the budget allows. Freeing an object refunds it. */
struct factory {
    struct kobj obj;
    uint32_t    budget;
};

void object_init(void);

/* Create an object with one reference (the caller's). `from` pays for it;
 * NULL means the kernel itself. Returns NULL if out of memory or budget. */
struct kobj *kobj_create(enum kobj_type type, struct factory *from);
struct kobj *factory_create(uint32_t budget);

void kobj_get(struct kobj *o);
void kobj_put(struct kobj *o);

const char *kobj_type_name(uint32_t type);