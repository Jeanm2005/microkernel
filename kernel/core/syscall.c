/* System-call dispatch. The architecture code saved the user registers
 * and calls syscall_handle() with interrupts enabled. */
#include <abi/syscall.h>
#include <kernel/cap.h>
#include <kernel/ipc.h>
#include <kernel/object.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>

static int64_t obj_create(struct cspace *cs, int64_t factory_slot, uint64_t type)
{
    struct cap *fc;
    int64_t err = cspace_lookup(cs, factory_slot, KOBJ_FACTORY, CAP_WRITE, &fc);
    if (err)
        return err;
    if (type != OBJ_ENDPOINT && type != OBJ_NOTIFICATION)
        return -ERR_INVAL;
    struct kobj *o = kobj_create((enum kobj_type)type, (struct factory *)fc->obj);
    if (!o)
        return -ERR_NOMEM;
    int64_t slot = cspace_insert(cs, o, CAP_ALL, 0);
    kobj_put(o);   /* the capability now holds the only reference */
    return slot;
}

uint64_t syscall_handle(struct syscall_regs *r)
{
    uint64_t *a = r->arg;
    struct cspace *cs = &thread_current()->proc->cspace;

    switch (r->nr) {
    case SYS_DEBUG_WRITE: return (uint64_t)process_debug_write(a[0], a[1]);
    case SYS_EXIT:        process_exit_current((int)a[0]);
    case SYS_YIELD:       thread_yield(); return 0;

    case SYS_SEND:        return ipc_send(r, false);
    case SYS_CALL:        return ipc_send(r, true);
    case SYS_RECV:        return ipc_recv(r);
    case SYS_REPLY:       return ipc_reply(r, false);
    case SYS_REPLY_RECV:  return ipc_reply(r, true);
    case SYS_NOTIFY:      return ipc_notify(r);
    case SYS_WAIT:        return ipc_wait(r);

    case SYS_CAP_COPY:    return (uint64_t)cspace_copy(cs, (int64_t)a[0], (uint32_t)a[1], a[2]);
    case SYS_CAP_DELETE:  return (uint64_t)cspace_delete(cs, (int64_t)a[0]);
    case SYS_OBJ_CREATE:  return (uint64_t)obj_create(cs, (int64_t)a[0], a[1]);

    default:              return (uint64_t)-ERR_NOSYS;
    }
}