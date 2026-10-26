/* What a program finds in its capability table when it starts.
 *
 * The root task gets its capabilities from the kernel; every other
 * program gets them from the root task, as its manifest line says. */
#pragma once

/* The root task. */
#define ROOT_TASK_PRIORITY  250   /* programs it starts must be at or below this */
#define ROOT_SLOT_FACTORY   1     /* creates kernel objects; has the whole budget */
#define ROOT_SLOT_IOPORTS   2     /* every I/O port, 0-0xffff */
#define ROOT_SLOT_IRQ_CONTROL 3   /* claims interrupt lines */
#define ROOT_SLOT_MODULES   4     /* one READ cap per boot module, in load order,
                                     up to the first empty slot */

/* Programs started from a manifest. */
#define SLOT_SERVE    1   /* the endpoint it serves (READ|GRANT), if it provides a service */
#define SLOT_USES     2   /* first of the services it uses (WRITE|GRANT, badged), in
                             the order its manifest line lists them (at most 4) */
#define SLOT_FACTORY  7   /* its own factory, if its manifest line gives it a quota */
#define SLOT_EVENTS   8   /* a notification of its own, if it has devices */
#define SLOT_DEVICES  9   /* its devices' capabilities, in manifest order:
                               com1: the I/O ports, then the IRQ
                               pci:  ports 0xcf8-0xcff (configuration), ports
                                     0x1000-0xffff (device registers), and
                                     IRQ control */