/* Shared between the root task's source files. */
#pragma once
#include <stdint.h>

uint64_t rdtsc(void);

int ipc_server(long mode);
int ipc_client(long mode);
int init_main(long mode);   /* init.c: start the system from a manifest */