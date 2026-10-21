/* Client side of the console protocol (abi/console.h). */
#pragma once
#include <stddef.h>

/* Send `len` bytes to the console server through endpoint capability `ep`,
 * in as many messages as it takes. Returns 0 or -ERR_*. */
long console_write(long ep, const char *s, size_t len);
long console_puts(long ep, const char *s);