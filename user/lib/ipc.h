/* IPC and capability system calls for user programs. Register usage is
 * described in abi/syscall.h. */
#pragma once
#include <abi/syscall.h>

struct msg {
    long w[4];     /* message words */
    long badge;    /* badge of the sender's capability (0 for replies) */
    long cap;      /* slot of a capability that came with it, 0 if none */
};

/* Raw syscall with all six argument registers as inputs and outputs. */
static inline long syscall6(long nr, long a[6])
{
    register long rdi __asm__("rdi") = a[0];
    register long rsi __asm__("rsi") = a[1];
    register long rdx __asm__("rdx") = a[2];
    register long r10 __asm__("r10") = a[3];
    register long r8  __asm__("r8")  = a[4];
    register long r9  __asm__("r9")  = a[5];
    __asm__ volatile("syscall"
                     : "+a"(nr), "+r"(rdi), "+r"(rsi), "+r"(rdx),
                       "+r"(r10), "+r"(r8), "+r"(r9)
                     :
                     : "rcx", "r11", "memory");
    a[0] = rdi; a[1] = rsi; a[2] = rdx; a[3] = r10; a[4] = r8; a[5] = r9;
    return nr;
}

static inline void msg_from_regs(struct msg *m, const long a[6])
{
    for (int i = 0; i < 4; i++)
        m->w[i] = a[i];
    m->badge = a[4];
    m->cap = a[5];
}

/* Send m->w (and optionally the cap in slot send_cap), wait for the
 * reply, and put the reply in *m. */
static inline long sys_call(long ep, struct msg *m, long send_cap)
{
    long a[6] = { ep, m->w[0], m->w[1], m->w[2], m->w[3], send_cap };
    long r = syscall6(SYS_CALL, a);
    if (r == 0)
        msg_from_regs(m, a);
    return r;
}

static inline long sys_send(long ep, const long w[4], long send_cap)
{
    long a[6] = { ep, w[0], w[1], w[2], w[3], send_cap };
    return syscall6(SYS_SEND, a);
}

static inline long sys_recv(long ep, struct msg *out)
{
    long a[6] = { ep, 0, 0, 0, 0, 0 };
    long r = syscall6(SYS_RECV, a);
    if (r == 0)
        msg_from_regs(out, a);
    return r;
}

static inline long sys_reply(const long w[4], long send_cap)
{
    long a[6] = { 0, w[0], w[1], w[2], w[3], send_cap };
    return syscall6(SYS_REPLY, a);
}

/* A server's main loop call: answer the current caller, then wait for the
 * next message. */
static inline long sys_reply_recv(long ep, const long w[4], long send_cap, struct msg *out)
{
    long a[6] = { ep, w[0], w[1], w[2], w[3], send_cap };
    long r = syscall6(SYS_REPLY_RECV, a);
    if (r == 0)
        msg_from_regs(out, a);
    return r;
}

static inline long sys_notify(long ntfn, long bits)
{
    long a[6] = { ntfn, bits, 0, 0, 0, 0 };
    return syscall6(SYS_NOTIFY, a);
}

static inline long sys_wait(long ntfn, long *bits)
{
    long a[6] = { ntfn, 0, 0, 0, 0, 0 };
    long r = syscall6(SYS_WAIT, a);
    if (r == 0)
        *bits = a[0];
    return r;
}

static inline long sys_cap_copy(long slot, long rights, long badge)
{
    long a[6] = { slot, rights, badge, 0, 0, 0 };
    return syscall6(SYS_CAP_COPY, a);
}

static inline long sys_cap_delete(long slot)
{
    long a[6] = { slot, 0, 0, 0, 0, 0 };
    return syscall6(SYS_CAP_DELETE, a);
}

static inline long sys_obj_create(long factory, long type)
{
    long a[6] = { factory, type, 0, 0, 0, 0 };
    return syscall6(SYS_OBJ_CREATE, a);
}