/* What a program finds in its capability table when it starts.
 *
 * The root task gets its capabilities from the kernel; every other
 * program gets them from the root task, as its manifest line says. */
 #pragma once

 /* The root task. */
 #define ROOT_TASK_PRIORITY 250 /* programs it starts must be at or below this */
 #define ROOT_SLOT_FACTORY 1 /* creates endpoints, notifications, processes */
 #define ROOT_SLOT_MODULES 2 /* one READ cap per boot module, in load order, up to the first empty slot */

 /* Programs started from a manifest */
 #define SLOT_SERVE 1 /* the endpoint it serves (READ|GRANT), if it provides a service */
 #define SLOT_USES 2 /* first of the services it uses (WRITE|GRANT, badged), in the order its manifest line lists them */