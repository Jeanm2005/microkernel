#include <arch/cpu.h>
#include <arch/paging.h>
#include <arch/user.h>
#include <abi/syscall.h>
#include <kernel/elf.h>
#include <kernel/ipc.h>
#include <kernel/kprintf.h>
#include <kernel/mm.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/slab.h>
#include <kernel/string.h>

static struct slab_cache process_cache;
static bool cache_ready;

/* First code of every user thread, in the kernel. schedule() has already
 * switched to the process's address space because thread->proc is set. */
static void user_thread_start(void *arg)
{
    struct process *p = arg;
    arch_enter_user(p->entry, USER_STACK_TOP, p->arg);
}

static bool map_stack(uint64_t root)
{
    for (uint64_t va = USER_STACK_TOP - USER_STACK_SIZE; va < USER_STACK_TOP; va += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_zeroed();
        if (frame == PMM_NONE)
            return false;
        if (!paging_map(root, va, frame, PAGE_USER | PAGE_WRITE)) {
            pmm_free(frame);
            return false;
        }
    }
    return true;
}

static void free_process(struct process *p)
{
    slab_free(&process_cache, p);
}

void process_put(struct process *p)
{
    uint64_t flags = irq_save();
    bool last = --p->refs == 0;
    irq_restore(flags);
    if (last)
        free_process(p);
}

struct process *process_create(const char *name, const void *elf, size_t size,
                               uint64_t arg, uint8_t priority)
{
    if (!cache_ready) {
        slab_cache_init(&process_cache, "process", sizeof(struct process));
        cache_ready = true;
    }
    struct process *p = slab_alloc(&process_cache);
    if (!p)
        return NULL;
    size_t n = strlen(name);
    if (n >= sizeof p->name)
        n = sizeof p->name - 1;
    memcpy(p->name, name, n);
    p->arg = arg;
    p->at_line_start = true;

    p->root = paging_new_root();
    if (p->root == PMM_NONE) {
        kprintf("process: %s: out of memory\n", name);
        goto fail_obj;
    }
    int err = elf_load(p->root, elf, size, &p->entry);
    if (err) {
        kprintf("process: %s: bad ELF image: %s\n", name, elf_strerror(err));
        goto fail_root;
    }
    if (!map_stack(p->root)) {
        kprintf("process: %s: out of memory for the stack\n", name);
        goto fail_root;
    }

    /* Two references: the thread's (dropped when it's reaped) and the
     * caller's. */
    p->refs = 2;
    p->thread = thread_new_user(p->name, user_thread_start, p, priority, p);
    if (!p->thread) {
        kprintf("process: %s: out of memory for the thread\n", name);
        goto fail_root;
    }
    return p;

fail_root:
    paging_destroy_root(p->root, true);
fail_obj:
    free_process(p);
    return NULL;
}

void process_start(struct process *p)
{
    thread_start(p->thread);
}

struct process *process_spawn(const char *name, const void *elf, size_t size,
                              uint64_t arg, uint8_t priority)
{
    struct process *p = process_create(name, elf, size, arg, priority);
    if (p)
        process_start(p);
    return p;
}

int64_t process_give_cap(struct process *p, struct kobj *obj, uint32_t rights, uint64_t badge)
{
    return cspace_insert(&p->cspace, obj, rights, badge);
}

void process_wait(struct process *p)
{
    uint64_t flags = irq_save();
    while (!p->exited) {
        waitq_push(&p->exit_waiters, thread_current());
        sched_block();
    }
    irq_restore(flags);
}

/* Runs right after the process's (only) thread has been switched away
 * from for the last time, with interrupts off. */
void process_thread_gone(struct process *p)
{
    /* Its address space is no longer active and every frame in its user
     * half belongs to it alone, so the memory can go. Its capabilities go
     * too, which frees any object nobody else holds. */
    paging_destroy_root(p->root, true);
    p->root = 0;
    cspace_clear(&p->cspace);
    p->exited = true;
    struct thread *t;
    while ((t = waitq_pop(&p->exit_waiters)))
        sched_wake(t);
    process_put(p);
}

void process_exit_current(int code)
{
    struct process *p = thread_current()->proc;
    kassert(p);
    p->exit_code = code;
    ipc_thread_dying(thread_current());
    thread_exit();   /* process_thread_gone() runs once it's reaped */
}

void process_kill_current(const char *reason)
{
    struct process *p = thread_current()->proc;
    kassert(p);
    p->killed = true;
    p->kill_reason = reason;
    kprintf("process: killed '%s': %s\n", p->name, reason);
    ipc_thread_dying(thread_current());
    thread_exit();
}

int64_t process_debug_write(uint64_t user_buf, uint64_t len)
{
    struct process *p = thread_current()->proc;
    if (!p)
        return -ERR_INVAL;
    if (len > DEBUG_WRITE_MAX)
        return -ERR_INVAL;
    /* Never dereference a user pointer before checking it: a bad one
     * would fault inside the kernel. One thread per process means nothing
     * can unmap the buffer between this check and the copy. */
    if (!paging_user_range_ok(p->root, user_buf, len, false))
        return -ERR_FAULT;

    const char *src = (const char *)user_buf;
    char chunk[128];
    size_t used = 0;
    for (uint64_t i = 0; i < len; i++) {
        if (p->at_line_start) {
            kprintf("[%s] ", p->name);
            p->at_line_start = false;
        }
        char c = src[i];
        chunk[used++] = c;
        if (c == '\n')
            p->at_line_start = true;
        if (c == '\n' || used == sizeof chunk - 1 || i + 1 == len) {
            chunk[used] = '\0';
            kprintf("%s", chunk);
            used = 0;
        }
    }
    return (int64_t)len;
}