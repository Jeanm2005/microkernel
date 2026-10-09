/* Shared between the root task's source files. */
#pragma once
#include <stdint.h>

extern int failures;
void check(int ok, const char *what);
uint64_t rdtsc(void);

int ipc_server(long mode);
int ipc_client(long mode);