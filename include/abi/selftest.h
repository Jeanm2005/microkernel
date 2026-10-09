/* Modes the kernel can start the root task in, passed as main()'s
 * argument. Each non-zero mode makes the root task misbehave in one
 * specific way, so the tests can check that the kernel kills it cleanly
 * and keeps running. */
#pragma once

enum root_mode {
    ROOT_MODE_NORMAL     = 0,
    ROOT_MODE_PAGEFAULT  = 1,   /* write through a null pointer */
    ROOT_MODE_PRIVILEGED = 2,   /* execute `cli`, a ring-0-only instruction */
    ROOT_MODE_KERNEL_READ = 3,  /* read kernel memory */
};