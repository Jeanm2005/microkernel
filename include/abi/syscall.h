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
 * capability table. Slot 0 is always empty.
 *
 * A thread with a bound notification (SYS_NTFN_BIND) can be woken from
 * SYS_RECV / SYS_REPLY_RECV by that notification: the call then returns
 * IPC_NOTIFIED instead of 0, with the notification's bits in rdi. */
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

    /* Boot modules (read-only files the bootloader loaded). */
    SYS_MODULE_INFO = 13,  /* (module, name_buf, buf_len) -> size; name copied out */
    SYS_MODULE_READ = 14,  /* (module, offset, buf, len) -> bytes copied */

    /* Processes. */
    SYS_PROC_CREATE = 15,  /* (factory, module, priority, name, name_len) -> new slot */
    SYS_PROC_GIVE   = 16,  /* (proc, slot, dst_slot, rights, badge) -> 0; copy a cap into it */
    SYS_PROC_START  = 17,  /* (proc, arg) -> 0; main(arg) starts running */
    SYS_PROC_WAIT   = 18,  /* (proc) -> 0, exit code in rdi; -ERR_DEAD if it was killed */

    /* Devices and events. */
    SYS_IOPORT_ENABLE = 19, /* (ioport) -> 0; this process may now use those ports */
    SYS_IRQ_BIND    = 20,  /* (irq, ntfn, bits) -> 0; the IRQ signals ntfn with bits */
    SYS_IRQ_ACK     = 21,  /* (irq) -> 0; done with the device, deliver the next one */
    SYS_NTFN_BIND   = 22,  /* (ntfn) -> 0; recv also returns when ntfn is signalled */

    /* Memory. */
    SYS_FRAME_ALLOC = 23,  /* (factory, pages) -> new slot; zeroed, physically contiguous */
    SYS_FRAME_INFO  = 24,  /* (frame) -> physical address, pages in rdi */
    SYS_MAP         = 25,  /* (frame, vaddr, writable) -> 0; map it into this process */
    SYS_UNMAP       = 26,  /* (vaddr) -> 0; undo the SYS_MAP that started at vaddr */

    /* Handing out parts of what we hold. */
    SYS_FACTORY_SPLIT = 27, /* (factory, objects, pages) -> new factory with that much budget */
    SYS_IOPORT_SUB  = 28,  /* (factory, ioport, base, count) -> new slot for a sub-range */
    SYS_IRQ_GET     = 29,  /* (factory, irq_control, line) -> new IRQ slot; once per line */

    SYS_PROC_WATCH  = 30,  /* (proc, ntfn, bits) -> 0; notify after teardown; before start */
    SYS_PROC_POLL   = 31,  /* (proc) -> like wait, but -ERR_AGAIN while running */
    SYS_IRQ_UNBIND  = 32,  /* (irq) -> 0; mask line and release its notification */

    SYS_COUNT
};

/* Rights a capability can carry. A copy can only have fewer. */
#define CAP_READ    (1 << 0)   /* recv, wait, bind a notification, read a module,
                                  wait for a process */
#define CAP_WRITE   (1 << 1)   /* send/call, notify, create from a factory,
                                  give caps to or start a process, use I/O
                                  ports, bind or acknowledge an IRQ */
#define CAP_GRANT   (1 << 2)   /* caps may travel in messages over this endpoint */
#define CAP_ALL     (CAP_READ | CAP_WRITE | CAP_GRANT)

/* Kernel object types. Only endpoints and notifications can be made with
 * SYS_OBJ_CREATE; processes come from SYS_PROC_CREATE, and factories and
 * modules only from the kernel at boot. */
#define OBJ_ENDPOINT      1
#define OBJ_NOTIFICATION  2
#define OBJ_FACTORY       3
#define OBJ_MODULE        4
#define OBJ_PROCESS       5
#define OBJ_IOPORT        6   /* a range of I/O ports */
#define OBJ_IRQ           7   /* one hardware interrupt line */
#define OBJ_FRAME         8   /* physically contiguous RAM */
#define OBJ_IRQ_CONTROL   9   /* the right to claim interrupt lines */

/* SYS_MAP: where a process may map frames (above its program and below
 * its stack), and the largest frame SYS_FRAME_ALLOC makes. */
#define USER_MAP_BASE     0x0000000100000000ull   /* 4 GiB */
#define USER_MAP_END      0x00007f0000000000ull
#define FRAME_MAX_PAGES   64                      /* 256 KiB */

/* Returned by SYS_RECV and SYS_REPLY_RECV when the bound notification
 * woke the thread instead of a message. */
#define IPC_NOTIFIED      1

/* Errors (returned negated). */
#define ERR_PERM    1    /* the capability lacks a needed right, or the
                            process may not do this at all */
#define ERR_AGAIN   11   /* process has not exited yet */
#define ERR_BADCAP  9    /* no capability in that slot */
#define ERR_NOMEM   12   /* out of kernel memory or factory budget */
#define ERR_FAULT   14   /* a pointer argument isn't valid user memory */
#define ERR_EXIST   17   /* the destination slot is already in use */
#define ERR_INVAL   22   /* an argument is out of range */
#define ERR_FULL    28   /* the capability table is full */
#define ERR_DEAD    32   /* the other side of the call died */
#define ERR_NOSYS   38   /* no such system call */
#define ERR_TYPE    40   /* the capability is for the wrong kind of object */

/* Bytes one SYS_DEBUG_WRITE call will accept. */
#define DEBUG_WRITE_MAX 1024