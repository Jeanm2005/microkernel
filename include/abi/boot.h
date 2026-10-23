/* What a program finds in its capability table when it starts.
 *
 * The root task gets its capabilities from the kernel; every other
 * program gets them from the root task, as its manifest line says. */
 #pragma once

 /* The root task. */
 #define ROOT_TASK_PRIORITY 250 /* programs it starts must be at or below this */
 #define ROOT_SLOT_FACTORY  1 /* creates endpoints, notifications, processes */
 #define ROOT_SLOT_COM1     2 /* I/O ports 0x3f8-0x3ff (the COM1 UART) */
 #define ROOT_SLOT_COM1_IRQ 3 /* IRQ 4, COM1's interrupt line */
 #define ROOT_SLOT_MODULES  4 /* one READ cap per boot module, in load order, up to the first empty slot */

 /* Programs started from a manifest */
 #define SLOT_SERVE 1 /* the endpoint it serves (READ|GRANT), if it provides a service */
 #define SLOT_USES 2 /* first of the services it uses (WRITE|GRANT, badged), in the order its manifest line lists them */
 #define SLOT_EVENTS 8 /* a notification of its own, if it has devices */
 #define SLOT_DEVICES 9 /* its devices' capabilities, in manifest order; for com1: the I/O ports then the IRQ */
