#pragma once
#include <stdbool.h>
#include <stddef.h>

/* The kernel command line is space-separated words, each either `flag` or
 * `key=value`. Looks up `key`; on success copies its value (truncated to
 * fit) into `out` and returns true. A bare `flag` has the value "". */
bool cmdline_get(const char *cmdline, const char *key, char *out, size_t out_size);