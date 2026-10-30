/* Version 1 storage protocols. All byte payloads occupy reply words 1..3.
 * BLK_READ: w1=sector, w2=offset within sector, w3=count (1..24).
 * FS_READ: w1=file id (1), w2=byte offset, w3=count (1..24).
 * Replies: w0=byte count (0 at EOF) or -ERR_*, w1..3=zero-padded data.
 * FS_STAT: w1..3=NUL-terminated name (at most 23 bytes); reply w0=file id,
 * w1=size. BLK_SIZE returns sector count in w0. Unknown names: -ERR_INVAL.
 * Both services accept PROTO_SHUTDOWN only from badge 0. */
#pragma once
#include <abi/proto.h>
#include <abi/syscall.h>
#define BLK_READ 1
#define BLK_SIZE 2
#define FS_STAT 1
#define FS_READ 2
#define STORAGE_CHUNK 24
#define TINYFS_MAGIC 0x315346594e4954ull /* "TINYFS1" little endian */
/* Sector 0: u64 magic, u64 file size, char name[24], then zero padding.
 * The single read-only file starts at sector 1. */

/* System policy shared by supervisor and retrying filesystem. Repeated
 * failure is bounded over this boot, rather than an infinite crash loop. */
#define DISK_RESTART_LIMIT 3
#define DISK_REQUEST_TICKS CLOCK_HZ /* one second, including spurious IRQs */
#define BLK_EXIT_TIMEOUT 75
#define BLK_EXIT_IO 74
#define BLK_TEST_CRASH 1
#define BLK_TEST_STALL 2

#define BLK_TEST_STALL_LATE 3 /* time out after some file reads have succeeded */

#define BLK_TEST_STALL_BOOT 4 /* no completion during the startup DMA probe */

#define BLK_TEST_STALL_NOISY 5 /* spurious notification flood during a stalled request */

#define BLK_TEST_SPIN 6 /* spin after DMA submission; only the CPU watchdog can stop it */
