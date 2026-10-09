/* The system-call ABI (application binary interface), shared by the
 * kernel and user programs. Nothing here may depend on kernel internals.
 *
 * Calling convention (x86_64): `syscall` instruction, number in rax,
 * arguments in rdi, rsi, rdx, r10, r8, r9, result in rax. The CPU uses rcx
 * and r11 to remember the return address and flags, so those two are
 * clobbered. Negative results are errors (-ERR_*).
 *
 * IPC calls also return values in the argument registers:
 *
 *            rdi     rsi    rdx    r10    r8     r9
 *   in:      slot    w0     w1     w2     w3     cap to send (0 = none)
 *   out:     w0      w1     w2     w3     badge  cap received (slot, 0 =
 *                                                none, <0 = -ERR_FULL)
 *
 * A capability (cap) is named by its slot number in the calling process's
 * capability table. Slot 0 is always empty. */
#pragma once

enum syscall_nr {
    SYS_DEBUG_WRITE = 0,   /* (buf, len) -> bytes written */
    SYS_EXIT        = 1,   /* (code) -> does not return */
    SYS_YIELD       = 2,   /* () -> 0 */

    /* Endpoints: synchronous messages. */
    SYS_SEND        = 3,   /* (ep, w0-w3, cap) -> 0; waits for a receiver */
    SYS_RECV        = 4,   /* (ep) -> message; waits for a sender */
    SYS_CALL        = 5,   /* (ep, w0-w3, cap) -> reply; send, then wait for the reply */
    SYS_REPLY       = 6,   /* (-, w0-w3, cap) -> 0; answer the last call received */
    SYS_REPLY_RECV  = 7,   /* (ep, w0-w3, cap) -> next message; reply, then recv */

    /* Notifications: a word of bits, set by signallers, cleared by the waiter. */
    SYS_NOTIFY      = 8,   /* (ntfn, bits) -> 0; never blocks */
    SYS_WAIT        = 9,   /* (ntfn) -> 0, bits in rdi; waits until some bit is set */

    /* Capabilities. */
    SYS_CAP_COPY    = 10,  /* (slot, rights, badge) -> new slot */
    SYS_CAP_DELETE  = 11,  /* (slot) -> 0 */
    SYS_OBJ_CREATE  = 12,  /* (factory, type) -> new slot with all rights */

    SYS_COUNT
};

/* Rights a capability can carry. A copy can only have fewer. */
#define CAP_READ    (1 << 0)   /* recv on an endpoint, wait on a notification */
#define CAP_WRITE   (1 << 1)   /* send/call on an endpoint, notify */
#define CAP_GRANT   (1 << 2)   /* caps may travel in messages over this endpoint */
#define CAP_ALL     (CAP_READ | CAP_WRITE | CAP_GRANT)

/* Kernel object types (for SYS_OBJ_CREATE). */
#define OBJ_ENDPOINT      1
#define OBJ_NOTIFICATION  2

/* Errors (returned negated). */
#define ERR_PERM    1    /* the capability lacks a needed right */
#define ERR_BADCAP  9    /* no capability in that slot */
#define ERR_NOMEM   12   /* out of kernel memory or factory budget */
#define ERR_FAULT   14   /* a pointer argument isn't valid user memory */
#define ERR_INVAL   22   /* an argument is out of range */
#define ERR_FULL    28   /* the capability table is full */
#define ERR_DEAD    32   /* the other side of the call died */
#define ERR_NOSYS   38   /* no such system call */
#define ERR_TYPE    40   /* the capability is for the wrong kind of object */

/* Bytes one SYS_DEBUG_WRITE call will accept. */
#define DEBUG_WRITE_MAX 1024