/* How the kernel's self-tests start user programs: the mode is main()'s
 * argument, and the capabilities a test hands out sit in fixed slots. */
#pragma once

enum root_mode {
    ROOT_MODE_NORMAL      = 0,
    ROOT_MODE_PAGEFAULT   = 1,  /* write through a null pointer */
    ROOT_MODE_PRIVILEGED  = 2,  /* execute `cli`, a ring-0-only instruction */
    ROOT_MODE_KERNEL_READ = 3,  /* read kernel memory */

    /* IPC tests: one server process and one client process. */
    ROOT_MODE_IPC_SERVER        = 4,
    ROOT_MODE_IPC_CLIENT        = 5,
    ROOT_MODE_IPC_SERVER_CRASH  = 6,  /* server that crashes mid-conversation */
    ROOT_MODE_IPC_CLIENT_ORPHAN = 7,  /* client that expects -ERR_DEAD */
};

/* Capability slots the IPC tests set up before starting the programs. */
#define TEST_SLOT_EP       1   /* server: READ|GRANT; client: WRITE|GRANT, badged */
#define TEST_SLOT_FACTORY  2   /* server only */
#define TEST_CLIENT_BADGE  42

/* Message operations in the test protocol (word 0 of each request). */
enum test_op {
    OP_PING = 1,           /* w1 = n          -> w0 = n + 1, w1 = badge seen */
    OP_GET_NOTIFICATION,   /*                 -> a notification cap */
    OP_WAIT_SIGNAL,        /* server replies, then blocks in SYS_WAIT */
    OP_GET_BITS,           /*                 -> w0 = bits the wait returned */
    OP_QUIT,               /* server replies, then exits */
};

/* In ROOT_MODE_IPC_SERVER_CRASH the server dies on this ping. */
#define TEST_CRASH_AT 500