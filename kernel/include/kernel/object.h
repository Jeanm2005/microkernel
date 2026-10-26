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
    KOBJ_MODULE = 4,
    KOBJ_PROCESS = 5,
    KOBJ_IOPORT = 6,
    KOBJ_IRQ = 7,
    KOBJ_FRAME = 8,
    KOBJ_IRQ_CONTROL = 9,
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
    struct kobj    obj;
    uint64_t       word;      /* pending bits */
    struct waitq   waiters;
    struct thread *bound;     /* thread whose recv this can interrupt, or NULL */
};

/* The right to create kernel objects, with a budget. This replaces any
 * ambient "anyone may allocate" authority: a process can only make kernel
 * objects (and so consume kernel memory) if it holds a factory capability,
 * and only as many as the budget allows. Freeing an object refunds it.
 * Frames (RAM handed to user space) also draw on a separate page budget.
 * A factory can be split: the new one's budget is carved out of the old
 * one's and goes back to it when the new one is freed. */
struct factory {
    struct kobj obj;
    uint32_t    budget;   /* objects */
    uint32_t    pages;    /* 4 KiB pages of RAM for frames */
};

/* A boot module: a read-only file the bootloader loaded (a program image
 * or a manifest). Only the kernel makes these, once, at boot. */
struct module {
    struct kobj obj;
    const void *data;     /* in the HHDM; never freed (it's boot memory) */
    uint64_t    size;
    char        name[32];
};

/* A handle to a process. Holds a reference to the process; dropping the
 * last handle of a process that was never started destroys it. */
struct process_obj {
    struct kobj     obj;
    struct process *proc;
};

/* A range of I/O ports a holder may enable for its own process. */
struct ioport {
    struct kobj obj;
    uint32_t    base;
    uint32_t    count;
};

/* Physically contiguous RAM a holder may map into its own address space.
 * Each mapping holds a reference, so the memory outlives every mapping. */
struct frame {
    struct kobj obj;
    uint64_t    phys;
    uint32_t    pages;
};

/* One hardware interrupt line. While bound, each interrupt masks the line
 * and signals `ntfn` with `bits`; SYS_IRQ_ACK unmasks it. */
struct irq {
    struct kobj          obj;
    uint32_t             line;
    struct notification *ntfn;   /* holds a reference, or NULL if unbound */
    uint64_t             bits;
};

void object_init(void);

/* Create an object with one reference (the caller's). `from` pays for it;
 * NULL means the kernel itself. Returns NULL if out of memory or budget. */
struct kobj *kobj_create(enum kobj_type type, struct factory *from);
struct kobj *factory_create(uint32_t objects, uint32_t pages);
struct kobj *module_create(const char *name, const void *data, uint64_t size);
/* Device objects. The kernel makes the first ones at boot; sub-ranges and
 * IRQ lines are made from those (SYS_IOPORT_SUB, SYS_IRQ_GET). */
struct kobj *ioport_create(uint32_t base, uint32_t count, struct factory *from);
struct kobj *irq_control_create(void);

void kobj_get(struct kobj *o);
void kobj_put(struct kobj *o);

const char *kobj_type_name(uint32_t type);