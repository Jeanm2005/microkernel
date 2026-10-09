/* IPC (inter-process communication): endpoints and notifications. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct kobj;
struct thread;
struct syscall_regs;

/* A message in flight. */
struct ipc_msg {
    uint64_t     w[4];
    uint64_t     badge;       /* the sender's endpoint-cap badge */
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
    struct thread *reply_to;     /* caller waiting for our reply, if any */
    bool           reply_grants; /* ... and whether it accepts caps */
};

uint64_t ipc_send(struct syscall_regs *r, bool call);
uint64_t ipc_recv(struct syscall_regs *r);
uint64_t ipc_reply(struct syscall_regs *r, bool then_recv);
uint64_t ipc_notify(struct syscall_regs *r);
uint64_t ipc_wait(struct syscall_regs *r);

/* The current thread is about to die: fail the call it owes a reply to,
 * so that caller gets -ERR_DEAD instead of waiting forever. */
void ipc_thread_dying(struct thread *t);