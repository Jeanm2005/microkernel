/* Console server protocol. Text travels in the message words themselves,
 * up to 24 bytes per message; a client splits longer text. (Shared-memory
 * buffers come once processes can map memory.)
 *
 * The console prints each client's text a line at a time, prefixed with
 * the name the root task registered for that client's badge. */
#pragma once
#include <abi/proto.h>

#define CONSOLE_WRITE      1      /* w0 = CONSOLE_WRITE | len << 8, w1..w3 = bytes
                                     -> w0 = bytes accepted */
#define CONSOLE_SET_NAME   2      /* root task only: w0 = op | badge << 8,
                                     w1..w3 = name, NUL-padded -> w0 = 0 */
#define CONSOLE_STATS      3      /* -> w0 = input lines received, w1 = interrupts */
#define CONSOLE_WRITE_MAX  24
#define CONSOLE_NAME_MAX   23
#define CONSOLE_MAX_CLIENTS 16    /* badges 0..15 */
