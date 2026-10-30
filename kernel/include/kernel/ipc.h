/* IPC (inter-process communication): endpoints and notifications. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct kobj;
struct cap_node;
struct notification;
struct thread;
struct syscall_regs;

/* A message in flight. */
struct ipc_msg {
    uint64_t     w[4];
    uint64_t     badge;       /* the sender's endpoint-cap badge */
    struct cap_node *node;
    struct kobj *cap;         /* capability being transferred (holds a ref), or NULL */
    uint32_t     cap_rights;
    uint64_t     cap_badge;
};

enum ipc_op { IPC_NONE, IPC_SEND, IPC_CALL, IPC_RECV, IPC_WAIT };

/* Per-thread IPC bookkeeping, embedded in struct thread. */
struct ipc_state {
    enum ipc_op    op;           /* what the thread is blocked doing */
    struct kobj   *blocked_on;   /* object whose queue we're on (holds a ref) */
    struct ipc_msg out;          /* outgoing message while blocked as a sender */
    bool           call_grants;  /* our call allows caps in the reply */
    uint64_t       in[6];        /* results for the user's registers */
    int64_t        status;       /* 0 or -ERR_*, set by whoever wakes us */
    struct cap_node *authority;
    struct thread *active_next, *reply_from;
    struct thread *reply_to;     /* caller waiting for our reply, if any */
    bool           reply_grants; /* ... and whether it accepts caps */
    struct thread *timeout_next; /* separate from the object wait queue */
    uint64_t deadline;           /* absolute scheduler tick, 0 if not armed */
    struct cap_node *bound_authority;
    struct notification *bound_ntfn; /* can interrupt our recv (holds a ref) */
};

uint64_t ipc_send(struct syscall_regs *r, bool call, bool timed);
uint64_t ipc_recv(struct syscall_regs *r, bool timed);
uint64_t ipc_reply(struct syscall_regs *r, bool then_recv);
uint64_t ipc_notify(struct syscall_regs *r);
uint64_t ipc_wait(struct syscall_regs *r, bool timed);
void ipc_timer_tick(uint64_t now); /* timer IRQ, interrupts off */
uint64_t ipc_ntfn_bind(struct syscall_regs *r);

/* Set `bits` in `n` and wake whoever is waiting for it: a thread in
 * SYS_WAIT, or the bound thread blocked in recv. Interrupts off; safe to
 * call from an interrupt handler. */
void ntfn_signal(struct notification *n, uint64_t bits);

/* The current thread is about to die: fail the call it owes a reply to,
 * so that caller gets -ERR_DEAD instead of waiting forever, and unbind
 * its notification. */
void ipc_thread_dying(struct thread *t);

void ipc_revoke(void);
void ipc_cancel(struct thread *t, int64_t error);
