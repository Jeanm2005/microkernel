/* Console server protocol. Text travels in the message words themselves,
 * up to 24 bytes per message; a client splits longer text. (Shared-memory
 * buffers come once processes can map memory.) */
 #pragma once
 #include <abi/proto.h>

 #define CONSOLE_WRITE 1 /* w0 = CONSOLE_WRITE | len << 8, w1..w3 = bytes -> w0 = bytes accepted */

 #define CONSOLE_WRITE_MAX 24
 #define CONSOLE_MAX_CLIENTS 16 /* badges 0..15 */