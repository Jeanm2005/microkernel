/* Read a named file through the filesystem, with no disk capability. */
#include <abi/boot.h>
#include <abi/storage.h>
#include "ipc.h"
#include "ulib.h"
#define FS (SLOT_USES + 1)
static int request(long op, long a, long b, long c, long expected)
{
    struct msg m = { .w = { op, a, b, c } };
    return sys_call(FS, &m, 0) == 0 && m.w[0] == expected;
}
int main(void)
{
    printf_use_console(SLOT_USES);
    struct msg m = { .w = { FS_STAT } };
    memcpy(&m.w[1], "hello.txt", 10);
    long r = sys_call(FS, &m, 0);
    if (r || m.w[0] != 1 || m.w[1] < 0) {
        printf("cannot open hello.txt: error %ld\n", r ? r : m.w[0]);
        return 1;
    }
    long size = m.w[1], offset = 0;
    check(request(PROTO_SHUTDOWN, 0, 0, 0, -ERR_PERM), "filesystem refuses client shutdown");
    check(request(FS_READ, 2, 0, 24, -ERR_INVAL) &&
          request(FS_READ, 1, -1, 24, -ERR_INVAL) &&
          request(FS_READ, 1, 0, 25, -ERR_INVAL), "filesystem rejects invalid reads");
    m = (struct msg){ .w = { FS_STAT } };
    memcpy(&m.w[1], "missing", 8);
    check(sys_call(FS, &m, 0) == 0 && m.w[0] == -ERR_INVAL, "missing file is refused");
    printf("cat hello.txt (%ld bytes):\n", size);
    /* Collect a line for console output; file reads still use 24-byte IPC. */
    char line[128];
    unsigned n = 0;
    while (offset < size) {
        m = (struct msg){ .w = { FS_READ, 1, offset, STORAGE_CHUNK } };
        r = sys_call(FS, &m, 0);
        if (r || m.w[0] <= 0 || m.w[0] > STORAGE_CHUNK || m.w[0] > size - offset) {
            printf("cannot read hello.txt: error %ld\n", r ? r : m.w[0]);
            return 1;
        }
        char *bytes = (char *)&m.w[1];
        for (long i = 0; i < m.w[0]; i++) {
            line[n++] = bytes[i];
            if (bytes[i] == '\n' || n == sizeof line - 1) {
                line[n] = 0; printf("%s", line); n = 0;
            }
        }
        offset += m.w[0];
    }
    if (n) { line[n] = 0; printf("%s", line); }
    check(request(FS_READ, 1, size, 24, 0), "EOF returns zero bytes");
    check(offset == size, "read the complete file from disk through the filesystem");
    return failures ? 1 : 0;
}
