/* Building processes from user space.
 *
 * The kernel still does the ELF loading (the same untrusted-input loader
 * as for the root task), because user space can't map memory into another
 * address space yet. What moved to user space is the policy: which
 * programs run, at what priority, and with which capabilities. */
#include <abi/syscall.h>
#include <kernel/cap.h>
#include <kernel/object.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/spawn.h>
#include <kernel/string.h>

int64_t sys_module_info(struct cspace *cs, int64_t slot, uint64_t name_buf, uint64_t buf_len)
{
    struct cap *c;
    int64_t err = cspace_lookup(cs, slot, KOBJ_MODULE, CAP_READ, &c);
    if (err)
        return err;
    struct module *m = (struct module *)c->obj;
    if (name_buf) {
        uint64_t n = strlen(m->name) + 1;   /* with the terminating NUL */
        if (buf_len < n)
            return -ERR_INVAL;
        if ((err = copy_to_user(name_buf, m->name, n)))
            return err;
    }
    return (int64_t)m->size;
}

int64_t sys_module_read(struct cspace *cs, int64_t slot, uint64_t offset, uint64_t buf,
                        uint64_t len)
{
    struct cap *c;
    int64_t err = cspace_lookup(cs, slot, KOBJ_MODULE, CAP_READ, &c);
    if (err)
        return err;
    struct module *m = (struct module *)c->obj;
    if (offset > m->size)
        return -ERR_INVAL;
    if (len > m->size - offset)
        len = m->size - offset;
    if ((err = copy_to_user(buf, (const char *)m->data + offset, len)))
        return err;
    return (int64_t)len;
}

int64_t sys_proc_create(struct cspace *cs, int64_t factory_slot, int64_t mod_slot,
                        uint64_t priority, uint64_t name, uint64_t name_len)
{
    struct cap *fc, *mc;
    int64_t err = cspace_lookup(cs, factory_slot, KOBJ_FACTORY, CAP_WRITE, &fc);
    if (!err)
        err = cspace_lookup(cs, mod_slot, KOBJ_MODULE, CAP_READ, &mc);
    if (err)
        return err;
    /* Priority 0 belongs to the idle thread. A process can't create one
     * that outranks itself, so no one gains priority by spawning. */
    if (priority == 0 || priority > 255)
        return -ERR_INVAL;
    if (priority > thread_current()->priority)
        return -ERR_PERM;

    char buf[24];
    if (name_len == 0 || name_len >= sizeof buf)
        return -ERR_INVAL;
    if ((err = copy_from_user(buf, name, name_len)))
        return err;
    buf[name_len] = '\0';

    struct process_obj *po = (struct process_obj *)kobj_create(KOBJ_PROCESS,
                                                               (struct factory *)fc->obj);
    if (!po)
        return -ERR_NOMEM;
    struct module *m = (struct module *)mc->obj;
    po->proc = process_create(buf, m->data, m->size, 0, (uint8_t)priority);
    if (!po->proc) {
        kobj_put(&po->obj);
        return -ERR_INVAL;   /* bad image or out of memory; the reason was printed */
    }
    po->proc->may_debug_write = false;   /* it prints through the console */
    int64_t slot = cspace_insert(cs, &po->obj, CAP_ALL, 0);
    kobj_put(&po->obj);   /* the capability holds the only reference now */
    return slot;
}

static int64_t lookup_proc(struct cspace *cs, int64_t slot, uint32_t rights, struct process **out)
{
    struct cap *c;
    int64_t err = cspace_lookup(cs, slot, KOBJ_PROCESS, rights, &c);
    if (!err)
        *out = ((struct process_obj *)c->obj)->proc;
    return err;
}

int64_t sys_proc_give(struct cspace *cs, int64_t proc, int64_t slot, int64_t dst_slot,
                      uint64_t rights, uint64_t badge)
{
    struct process *p;
    int64_t err = lookup_proc(cs, proc, CAP_WRITE, &p);
    if (err)
        return err;
    if (p->exited)
        return -ERR_DEAD;
    if (dst_slot <= 0)
        return -ERR_INVAL;
    err = cspace_derive(cs, slot, &p->cspace, dst_slot, (uint32_t)rights, badge);
    return err < 0 ? err : 0;
}

int64_t sys_proc_start(struct cspace *cs, int64_t proc, uint64_t arg)
{
    struct process *p;
    int64_t err = lookup_proc(cs, proc, CAP_WRITE, &p);
    if (err)
        return err;
    if (p->started)
        return -ERR_INVAL;
    p->arg = arg;
    process_start(p);
    return 0;
}

int64_t sys_proc_wait(struct cspace *cs, int64_t proc, uint64_t *code)
{
    struct process *p;
    int64_t err = lookup_proc(cs, proc, CAP_READ, &p);
    if (err)
        return err;
    if (!p->started)
        return -ERR_INVAL;   /* would wait forever */
    /* The capability we just looked up keeps p alive while we wait: only
     * this (single-threaded) process could delete it, and it's blocked. */
    process_wait(p);
    if (p->killed)
        return -ERR_DEAD;
    *code = (uint64_t)(int64_t)p->exit_code;
    return 0;
}
