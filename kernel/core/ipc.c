/* Endpoints and notifications.
 *
 * Every path here runs with interrupts off: on one CPU that makes each
 * operation atomic with respect to every other thread and interrupt.
 *
 * A message is copied once, register to register: the sender's words go
 * into the receiver's `ipc.in`, which becomes its syscall results. If a
 * receiver is already waiting, a call switches straight to it
 * (sched_block_and_switch) without touching the run queue; that is the
 * L4 IPC fast path, and reply_recv does the same back to the caller. */
#include <arch/cpu.h>
#include <abi/syscall.h>
#include <kernel/cap.h>
#include <kernel/ipc.h>
#include <kernel/object.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/syscall.h>

static struct cspace *my_cspace(void)
{
    return &thread_current()->proc->cspace;
}

/* Build an outgoing message from the syscall registers (words in arg[1..4],
 * a capability slot to send in arg[5]). The capability is referenced, not
 * moved: the sender keeps its own copy. */
static int64_t build_msg(struct syscall_regs *r, uint64_t badge, bool caps_allowed,
                         struct ipc_msg *m)
{
    memset(m, 0, sizeof *m);
    for (int i = 0; i < 4; i++)
        m->w[i] = r->arg[1 + i];
    m->badge = badge;
    if (r->arg[5]) {
        if (!caps_allowed)
            return -ERR_PERM;
        struct cap *c;
        int64_t err = cspace_lookup(my_cspace(), (int64_t)r->arg[5], 0, 0, &c);
        if (err)
            return err;
        kobj_get(c->obj);
        m->cap = c->obj;
        m->cap_rights = c->rights;
        m->cap_badge = c->badge;
    }
    return 0;
}

/* Hand a message to `to` (blocked, not running): words and badge into its
 * results, and any capability into its table. */
static void deliver(struct thread *to, struct ipc_msg *m)
{
    struct ipc_state *s = &to->ipc;
    for (int i = 0; i < 4; i++)
        s->in[i] = m->w[i];
    s->in[4] = m->badge;
    s->in[5] = 0;
    if (m->cap) {
        s->in[5] = (uint64_t)cspace_insert(&to->proc->cspace, m->cap,
                                           m->cap_rights, m->cap_badge);
        kobj_put(m->cap);   /* insert took its own reference */
        m->cap = NULL;
    }
    s->status = 0;
}

static void unblock_from_object(struct thread *t)
{
    if (t->ipc.blocked_on) {
        kobj_put(t->ipc.blocked_on);   /* never the last ref: a cap holds one */
        t->ipc.blocked_on = NULL;
    }
    t->ipc.op = IPC_NONE;
}

static void block_on(struct thread *me, struct kobj *o, struct waitq *q, enum ipc_op op)
{
    me->ipc.op = op;
    kobj_get(o);   /* keeps the object alive while we wait on it */
    me->ipc.blocked_on = o;
    waitq_push(q, me);
}

static uint64_t results(struct syscall_regs *r, struct thread *me)
{
    if (me->ipc.status == 0)
        for (int i = 0; i < 6; i++)
            r->arg[i] = me->ipc.in[i];
    return (uint64_t)me->ipc.status;
}

/* A server that receives again without replying abandons its caller. */
static void abandon_reply(struct thread *me)
{
    struct thread *caller = me->ipc.reply_to;
    if (!caller)
        return;
    me->ipc.reply_to = NULL;
    memset(caller->ipc.in, 0, sizeof caller->ipc.in);
    caller->ipc.status = -ERR_DEAD;
    caller->ipc.op = IPC_NONE;
    sched_wake(caller);
}

void ipc_thread_dying(struct thread *t)
{
    uint64_t flags = irq_save();
    abandon_reply(t);
    irq_restore(flags);
}

uint64_t ipc_send(struct syscall_regs *r, bool call)
{
    uint64_t flags = irq_save();
    struct thread *me = thread_current();
    struct cap *epc;
    struct ipc_msg m;
    int64_t err = cspace_lookup(my_cspace(), (int64_t)r->arg[0], KOBJ_ENDPOINT, CAP_WRITE, &epc);
    if (!err)
        err = build_msg(r, epc->badge, epc->rights & CAP_GRANT, &m);
    if (err) {
        irq_restore(flags);
        return (uint64_t)err;
    }
    struct endpoint *ep = (struct endpoint *)epc->obj;
    bool grants = epc->rights & CAP_GRANT;

    struct thread *rx = waitq_pop(&ep->receivers);
    if (rx) {
        /* A receiver is waiting: give it the message now. */
        deliver(rx, &m);
        unblock_from_object(rx);
        if (call) {
            rx->ipc.reply_to = me;
            rx->ipc.reply_grants = grants;
            me->ipc.op = IPC_CALL;
            sched_block_and_switch(rx);   /* back here once replied to */
            uint64_t ret = results(r, me);
            irq_restore(flags);
            return ret;
        }
        sched_wake(rx);
        irq_restore(flags);
        return 0;
    }

    /* Nobody listening yet: wait in line with the message. */
    me->ipc.out = m;
    me->ipc.call_grants = grants;
    block_on(me, &ep->obj, &ep->senders, call ? IPC_CALL : IPC_SEND);
    sched_block();
    uint64_t ret = call ? results(r, me) : (uint64_t)me->ipc.status;
    irq_restore(flags);
    return ret;
}

/* Take a message from `ep`, waiting if there is none. Interrupts off. */
static void do_recv(struct thread *me, struct endpoint *ep)
{
    struct thread *tx = waitq_pop(&ep->senders);
    if (!tx) {
        block_on(me, &ep->obj, &ep->receivers, IPC_RECV);
        sched_block();
        return;   /* the sender delivered into me->ipc */
    }

    deliver(me, &tx->ipc.out);
    bool is_call = tx->ipc.op == IPC_CALL;
    unblock_from_object(tx);
    if (is_call) {
        /* The caller stays blocked until we reply. */
        tx->ipc.op = IPC_CALL;
        me->ipc.reply_to = tx;
        me->ipc.reply_grants = tx->ipc.call_grants;
    } else {
        tx->ipc.status = 0;
        sched_wake(tx);
    }
}

uint64_t ipc_recv(struct syscall_regs *r)
{
    uint64_t flags = irq_save();
    struct thread *me = thread_current();
    struct cap *epc;
    int64_t err = cspace_lookup(my_cspace(), (int64_t)r->arg[0], KOBJ_ENDPOINT, CAP_READ, &epc);
    if (err) {
        irq_restore(flags);
        return (uint64_t)err;
    }
    abandon_reply(me);
    do_recv(me, (struct endpoint *)epc->obj);
    uint64_t ret = results(r, me);
    irq_restore(flags);
    return ret;
}

uint64_t ipc_reply(struct syscall_regs *r, bool then_recv)
{
    uint64_t flags = irq_save();
    struct thread *me = thread_current();
    struct endpoint *ep = NULL;
    int64_t err = 0;

    if (then_recv) {
        struct cap *epc;
        err = cspace_lookup(my_cspace(), (int64_t)r->arg[0], KOBJ_ENDPOINT, CAP_READ, &epc);
        if (!err)
            ep = (struct endpoint *)epc->obj;
    } else if (!me->ipc.reply_to) {
        err = -ERR_INVAL;   /* nothing to reply to */
    }

    struct thread *caller = me->ipc.reply_to;
    struct ipc_msg m;
    if (!err && caller)
        err = build_msg(r, 0, me->ipc.reply_grants, &m);
    if (err) {
        irq_restore(flags);
        return (uint64_t)err;
    }

    if (caller) {
        me->ipc.reply_to = NULL;
        deliver(caller, &m);
        caller->ipc.op = IPC_NONE;
    }

    if (!then_recv) {
        sched_wake(caller);
        irq_restore(flags);
        return 0;
    }

    if (caller && waitq_empty(&ep->senders)) {
        /* Fast path: start waiting for the next message and run the
         * caller right away. */
        block_on(me, &ep->obj, &ep->receivers, IPC_RECV);
        sched_block_and_switch(caller);
    } else {
        if (caller)
            sched_wake(caller);
        do_recv(me, ep);
    }
    uint64_t ret = results(r, me);
    irq_restore(flags);
    return ret;
}

uint64_t ipc_notify(struct syscall_regs *r)
{
    uint64_t flags = irq_save();
    struct cap *c;
    int64_t err = cspace_lookup(my_cspace(), (int64_t)r->arg[0], KOBJ_NOTIFICATION, CAP_WRITE, &c);
    if (err) {
        irq_restore(flags);
        return (uint64_t)err;
    }
    struct notification *n = (struct notification *)c->obj;
    n->word |= r->arg[1] | c->badge;
    if (n->word) {
        struct thread *t = waitq_pop(&n->waiters);
        if (t) {
            t->ipc.in[0] = n->word;
            n->word = 0;
            t->ipc.status = 0;
            unblock_from_object(t);
            sched_wake(t);
        }
    }
    irq_restore(flags);
    return 0;
}

uint64_t ipc_wait(struct syscall_regs *r)
{
    uint64_t flags = irq_save();
    struct thread *me = thread_current();
    struct cap *c;
    int64_t err = cspace_lookup(my_cspace(), (int64_t)r->arg[0], KOBJ_NOTIFICATION, CAP_READ, &c);
    if (err) {
        irq_restore(flags);
        return (uint64_t)err;
    }
    struct notification *n = (struct notification *)c->obj;
    if (n->word) {
        r->arg[0] = n->word;
        n->word = 0;
    } else {
        block_on(me, &n->obj, &n->waiters, IPC_WAIT);
        sched_block();
        r->arg[0] = me->ipc.in[0];
    }
    irq_restore(flags);
    return 0;
}