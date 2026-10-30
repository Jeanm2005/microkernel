/* System-call dispatch. The architecture code saved the user registers
 * and calls syscall_handle() with interrupts enabled. */
#include <abi/syscall.h>
#include <kernel/cap.h>
#include <kernel/ipc.h>
#include <kernel/irq.h>
#include <kernel/memory.h>
#include <kernel/object.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/spawn.h>
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

static uint64_t dispatch(struct syscall_regs *r)
{
    uint64_t *a = r->arg;
    struct cspace *cs = &thread_current()->proc->cspace;

    switch (r->nr) {
    case SYS_DEBUG_WRITE: return (uint64_t)process_debug_write(a[0], a[1]);
    case SYS_EXIT:        process_exit_current((int)a[0]);
    case SYS_YIELD:       thread_yield(); return 0;

    case SYS_SEND:        return ipc_send(r, false, false);
    case SYS_CALL:        return ipc_send(r, true, false);
    case SYS_RECV:        return ipc_recv(r, false);
    case SYS_REPLY:       return ipc_reply(r, false);
    case SYS_REPLY_RECV:  return ipc_reply(r, true);
    case SYS_NOTIFY:      return ipc_notify(r);
    case SYS_WAIT:        return ipc_wait(r, false);

    case SYS_CAP_COPY:    return (uint64_t)cspace_copy(cs, (int64_t)a[0], (uint32_t)a[1], a[2]);
    case SYS_CAP_DELETE:  return (uint64_t)cspace_delete(cs, (int64_t)a[0]);
    case SYS_OBJ_CREATE:  return (uint64_t)obj_create(cs, (int64_t)a[0], a[1]);

    case SYS_MODULE_INFO: return (uint64_t)sys_module_info(cs, (int64_t)a[0], a[1], a[2]);
    case SYS_MODULE_READ: return (uint64_t)sys_module_read(cs, (int64_t)a[0], a[1], a[2], a[3]);
    case SYS_PROC_CREATE: return (uint64_t)sys_proc_create(cs, (int64_t)a[0], (int64_t)a[1],
                                                           a[2], a[3], a[4]);
    case SYS_PROC_GIVE:   return (uint64_t)sys_proc_give(cs, (int64_t)a[0], (int64_t)a[1],
                                                         (int64_t)a[2], a[3], a[4]);
    case SYS_PROC_START:  return (uint64_t)sys_proc_start(cs, (int64_t)a[0], a[1]);
    case SYS_PROC_WAIT:   return (uint64_t)sys_proc_wait(cs, (int64_t)a[0], &a[0]);

    case SYS_IOPORT_ENABLE: return (uint64_t)sys_ioport_enable(cs, (int64_t)a[0]);
    case SYS_IRQ_BIND:    return (uint64_t)sys_irq_bind(cs, (int64_t)a[0], (int64_t)a[1], a[2]);
    case SYS_IRQ_ACK:     return (uint64_t)sys_irq_ack(cs, (int64_t)a[0]);
    case SYS_NTFN_BIND:   return ipc_ntfn_bind(r);

    case SYS_FRAME_ALLOC: return (uint64_t)sys_frame_alloc(cs, (int64_t)a[0], a[1]);
    case SYS_FRAME_INFO:  return (uint64_t)sys_frame_info(cs, (int64_t)a[0], &a[0]);
    case SYS_MAP:         return (uint64_t)sys_map(cs, (int64_t)a[0], a[1], a[2]);
    case SYS_UNMAP:       return (uint64_t)sys_unmap(a[0]);
    case SYS_FACTORY_SPLIT: return (uint64_t)sys_factory_split(cs, (int64_t)a[0], a[1], a[2]);
    case SYS_IOPORT_SUB:  return (uint64_t)sys_ioport_sub(cs, (int64_t)a[0], (int64_t)a[1],
                                                          a[2], a[3]);
    case SYS_IRQ_GET:     return (uint64_t)sys_irq_get(cs, (int64_t)a[0], (int64_t)a[1], a[2]);

    case SYS_PROC_WATCH: return (uint64_t)sys_proc_watch(cs, (int64_t)a[0], (int64_t)a[1], a[2]);
    case SYS_PROC_POLL: return (uint64_t)sys_proc_poll(cs, (int64_t)a[0], &a[0]);
    case SYS_IRQ_UNBIND: return (uint64_t)sys_irq_unbind(cs, (int64_t)a[0]);

    case SYS_WAIT_UNTIL: return ipc_wait(r, true);
    case SYS_CLOCK: return sched_ticks();

    case SYS_PROC_WATCHDOG: return (uint64_t)sys_proc_watchdog(cs, (int64_t)a[0], a[1]);

    case SYS_CALL_UNTIL: return ipc_send(r, true, true);
    case SYS_RECV_UNTIL: return ipc_recv(r, true);
    case SYS_CAP_REVOKE: return (uint64_t)cspace_revoke(cs, (int64_t)a[0]);
    case SYS_PROC_STOP: return (uint64_t)sys_proc_stop(cs, (int64_t)a[0]);
    default:              return (uint64_t)-ERR_NOSYS;
    }
}

uint64_t syscall_handle(struct syscall_regs *r)
{
    process_watchdog_check();
    process_watchdog_pause();
    uint64_t result = dispatch(r);
    process_watchdog_check();
    process_watchdog_resume();
    return result;
}
