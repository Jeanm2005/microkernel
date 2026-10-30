/* Endpoints and notifications.
 *
 * Every path here runs with interrupts off: on one CPU that makes each
 * operation atomic with respect to every other thread and interrupt.
 *
 * A message is copied once, register to register: the sender's words go
 * into the receiver's `ipc.in`, which becomes its syscall results. If a
 * receiver is already waiting, a call switches straight to it
 * (sched_block_and_switch) without touching the run queue; that is the
 * L4 IPC fast path, and reply_recv does the same back to the caller.
 *
 * A thread may also have a bound notification. Signalling it wakes the
 * thread out of recv (status IPC_NOTIFIED, bits in word 0), so one server
 * thread can wait for client requests and device interrupts at once. */
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

/* Timed notification waiters also remain in the notification's FIFO.
 * A separate link lets the timer remove exactly one waiter atomically. */
static struct thread *timeouts, *active;

static void disarm_timeout(struct thread *t)
{
    if (!t->ipc.deadline)
        return;
    struct thread **link = &timeouts;
    while (*link != t) link = &(*link)->ipc.timeout_next;
    *link = t->ipc.timeout_next;
    t->ipc.timeout_next = NULL;
    t->ipc.deadline = 0;
}

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
        m->node = cap_node_new(c->obj, c->node);
        if (!m->node) return -ERR_NOMEM;
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
        s->in[5] = (uint64_t)cspace_insert_node(&to->proc->cspace, m->node,
                                           m->cap_rights, m->cap_badge, 0);
        cap_node_put(m->node);   /* insert took its own reference */
        m->cap = NULL; m->node = NULL;
    }
    s->status = 0;
}

static void unblock_from_object(struct thread *t)
{
    if (t->ipc.blocked_on) {
        kobj_put(t->ipc.blocked_on);   /* never the last ref: a cap holds one */
        t->ipc.blocked_on = NULL;
    }
}

static void block_on(struct thread *me, struct kobj *o, struct waitq *q, enum ipc_op op)
{
    me->ipc.op = op;
    kobj_get(o);   /* keeps the object alive while we wait on it */
    me->ipc.blocked_on = o;
    waitq_push(q, me);
}

static void hold_authority(struct thread *t, struct cap_node *node)
{
    cap_node_get(node); t->ipc.authority = node;
    t->ipc.active_next = active; active = t;
}
static void finish(struct thread *t)
{
    disarm_timeout(t);
    if (t->ipc.authority) {
        struct thread **link = &active;
        while (*link != t) link = &(*link)->ipc.active_next;
        *link = t->ipc.active_next;
        cap_node_put(t->ipc.authority); t->ipc.authority = NULL;
    }
    t->ipc.op = IPC_NONE;
}
static void arm(struct thread *t, uint64_t deadline)
{
    t->ipc.deadline = deadline;
    t->ipc.timeout_next = timeouts; timeouts = t;
}

static uint64_t results(struct syscall_regs *r, struct thread *me)
{
    if (me->ipc.status >= 0)   /* a message, or IPC_NOTIFIED */
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
    caller->ipc.reply_from = NULL;
    finish(caller);
    sched_wake(caller);
}

void ipc_thread_dying(struct thread *t)
{
    uint64_t flags = irq_save();
    abandon_reply(t);
    struct notification *n = t->ipc.bound_ntfn;
    if (n) {
        n->bound = NULL;
        t->ipc.bound_ntfn = NULL;
        cap_use_put(t->ipc.bound_authority); t->ipc.bound_authority = NULL;
    }
    irq_restore(flags);
    if (n)
        kobj_put(&n->obj);
}

/* If our bound notification has bits pending, take them as the result of
 * this recv. Interrupts off. */
static bool take_bound(struct thread *me)
{
    struct notification *n = me->ipc.bound_ntfn;
    if (!n || !n->word)
        return false;
    memset(me->ipc.in, 0, sizeof me->ipc.in);
    me->ipc.in[0] = n->word;
    n->word = 0;
    me->ipc.status = IPC_NOTIFIED;
    return true;
}

uint64_t ipc_send(struct syscall_regs *r, bool call, bool timed)
{
    uint64_t flags = irq_save();
    struct thread *me = thread_current();
    struct cap *epc;
    struct ipc_msg m;
    uint64_t deadline = timed ? r->arg[5] : 0;
    if (timed) r->arg[5] = 0;
    int64_t err = cspace_lookup(my_cspace(), (int64_t)r->arg[0], KOBJ_ENDPOINT, CAP_WRITE, &epc);
    if (!err && timed && deadline <= sched_ticks()) err = -ERR_TIMEDOUT;
    if (!err) err = build_msg(r, epc->badge, epc->rights & CAP_GRANT, &m);
    if (err) {
        irq_restore(flags);
        return (uint64_t)err;
    }
    struct endpoint *ep = (struct endpoint *)epc->obj;
    bool grants = epc->rights & CAP_GRANT;
    hold_authority(me, epc->node);
    if (timed) arm(me, deadline);

    struct thread *rx = waitq_pop(&ep->receivers);
    if (rx) {
        /* A receiver is waiting: give it the message now. */
        deliver(rx, &m);
        unblock_from_object(rx);
        finish(rx);
        if (call) {
            rx->ipc.reply_to = me;
            me->ipc.reply_from = rx;
            rx->ipc.reply_grants = grants;
            me->ipc.op = IPC_CALL;
            sched_block_and_switch(rx);   /* back here once replied to */
            uint64_t ret = results(r, me);
            irq_restore(flags);
            return ret;
        }
        finish(me);
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

/* Take a message from `ep`, waiting if there is none. Interrupts off.
 * Pending bound-notification bits win over waiting senders, so a busy
 * server can't starve its own device interrupts. */
static void do_recv(struct thread *me, struct endpoint *ep)
{
    if (take_bound(me))
        return;
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
        tx->ipc.reply_from = me;
        me->ipc.reply_grants = tx->ipc.call_grants;
    } else {
        tx->ipc.status = 0;
        finish(tx);
        sched_wake(tx);
    }
}

uint64_t ipc_recv(struct syscall_regs *r, bool timed)
{
    uint64_t flags = irq_save();
    struct thread *me = thread_current();
    struct cap *epc;
    int64_t err = cspace_lookup(my_cspace(), (int64_t)r->arg[0], KOBJ_ENDPOINT, CAP_READ, &epc);
    if (err) {
        irq_restore(flags);
        return (uint64_t)err;
    }
    if (timed && r->arg[1] <= sched_ticks()) { irq_restore(flags); return (uint64_t)-ERR_TIMEDOUT; }
    abandon_reply(me);
    hold_authority(me, epc->node);
    if (timed) arm(me, r->arg[1]);
    do_recv(me, (struct endpoint *)epc->obj);
    finish(me);
    uint64_t ret = results(r, me);
    irq_restore(flags);
    return ret;
}

uint64_t ipc_reply(struct syscall_regs *r, bool then_recv)
{
    uint64_t flags = irq_save();
    struct thread *me = thread_current();
    struct endpoint *ep = NULL;
    struct cap_node *recv_node = NULL;
    int64_t err = 0;

    if (then_recv) {
        struct cap *epc;
        err = cspace_lookup(my_cspace(), (int64_t)r->arg[0], KOBJ_ENDPOINT, CAP_READ, &epc);
        if (!err) { ep = (struct endpoint *)epc->obj; recv_node = epc->node; }
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
        caller->ipc.reply_from = NULL;
        finish(caller);
    }

    if (!then_recv) {
        sched_wake(caller);
        irq_restore(flags);
        return 0;
    }

    hold_authority(me, recv_node);
    if (caller && waitq_empty(&ep->senders) &&
        !(me->ipc.bound_ntfn && me->ipc.bound_ntfn->word)) {
        /* Fast path: start waiting for the next message and run the
         * caller right away. */
        block_on(me, &ep->obj, &ep->receivers, IPC_RECV);
        sched_block_and_switch(caller);
    } else {
        if (caller)
            sched_wake(caller);
        do_recv(me, ep);
    }
    finish(me);
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
    ntfn_signal((struct notification *)c->obj, r->arg[1] | c->badge);
    irq_restore(flags);
    return 0;
}

void ntfn_signal(struct notification *n, uint64_t bits)
{
    n->word |= bits;
    if (!n->word)
        return;

    /* The bound thread, if it's waiting for a message right now. */
    struct thread *b = n->bound;
    if (b && b->state == THREAD_BLOCKED && b->ipc.op == IPC_RECV) {
        struct endpoint *ep = (struct endpoint *)b->ipc.blocked_on;
        waitq_remove(&ep->receivers, b);
        memset(b->ipc.in, 0, sizeof b->ipc.in);
        b->ipc.in[0] = n->word;
        n->word = 0;
        b->ipc.status = IPC_NOTIFIED;
        unblock_from_object(b);
        finish(b);
        sched_wake(b);
        return;
    }

    struct thread *t = waitq_pop(&n->waiters);
    if (t) {
        t->ipc.in[0] = n->word;
        n->word = 0;
        t->ipc.status = 0;
        unblock_from_object(t);
        finish(t);
        sched_wake(t);
    }
}

uint64_t ipc_ntfn_bind(struct syscall_regs *r)
{
    uint64_t flags = irq_save();
    struct thread *me = thread_current();
    struct cap *c;
    int64_t err = cspace_lookup(my_cspace(), (int64_t)r->arg[0], KOBJ_NOTIFICATION, CAP_READ, &c);
    struct notification *n = err ? NULL : (struct notification *)c->obj;
    if (!err && (n->bound || me->ipc.bound_ntfn))
        err = -ERR_EXIST;   /* one notification per thread and vice versa */
    if (!err) {
        kobj_get(&n->obj);
        cap_use_get(c->node); me->ipc.bound_authority = c->node;
        n->bound = me;
        me->ipc.bound_ntfn = n;
    }
    irq_restore(flags);
    return (uint64_t)err;
}

uint64_t ipc_wait(struct syscall_regs *r, bool timed)
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
    } else if (timed && r->arg[1] <= sched_ticks()) {
        err = -ERR_TIMEDOUT;
    } else {
        hold_authority(me, c->node);
        block_on(me, &n->obj, &n->waiters, IPC_WAIT);
        if (timed) arm(me, r->arg[1]);
        sched_block();
        err = me->ipc.status;
        if (!err) r->arg[0] = me->ipc.in[0];
    }
    irq_restore(flags);
    return (uint64_t)err;
}

void ipc_cancel(struct thread *t, int64_t error)
{
    if (!t->ipc.authority || t->state != THREAD_BLOCKED) return;
    if (t->ipc.blocked_on) {
        if (t->ipc.op == IPC_WAIT)
            waitq_remove(&((struct notification *)t->ipc.blocked_on)->waiters, t);
        else {
            struct endpoint *ep = (struct endpoint *)t->ipc.blocked_on;
            waitq_remove(t->ipc.op == IPC_RECV ? &ep->receivers : &ep->senders, t);
        }
        unblock_from_object(t);
    } else if (t->ipc.reply_from) {
        t->ipc.reply_from->ipc.reply_to = NULL;
        t->ipc.reply_from = NULL;
    }
    if (t->ipc.out.node) { cap_node_put(t->ipc.out.node); t->ipc.out.node = NULL; t->ipc.out.cap = NULL; }
    finish(t); t->ipc.status = error;
    sched_wake(t);
}
void ipc_revoke(void)
{
    for (struct thread *t = active, *next; t; t = next) {
        next = t->ipc.active_next;
        if (!cap_node_valid(t->ipc.authority)) ipc_cancel(t, -ERR_REVOKED);
    }
}
void ipc_timer_tick(uint64_t now)
{
    for (struct thread *t = timeouts, *next; t; t = next) {
        next = t->ipc.timeout_next;
        if (t->ipc.deadline <= now) ipc_cancel(t, -ERR_TIMEDOUT);
    }
}
