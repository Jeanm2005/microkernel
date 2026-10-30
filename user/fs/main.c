/* A read-only, single-file filesystem. Disk metadata is untrusted. */
#include <stdint.h>
#include <abi/boot.h>
#include <abi/storage.h>
#include "ipc.h"
#include "ulib.h"
#define BLK (SLOT_USES + 1)

static long disk_read(uint64_t sector, long offset, long count, void *out)
{
    struct msg m;
    long r;
    /* Read retries are safe: there is no client write API. This per-read
     * retry bound complements root's boot-wide restart budget. */
    for (int retries = 0;; retries++) {
        m = (struct msg){ .w = { BLK_READ, (long)sector, offset, count } };
        r = sys_call(BLK, &m, 0);
        if (r != -ERR_DEAD || retries == DISK_RESTART_LIMIT)
            break;
        printf("disk driver died; retrying on its replacement\n");
    }
    if (r) return r == -ERR_DEAD ? -ERR_IO : r;
    if (m.w[0] != count) return m.w[0] < 0 ? m.w[0] : -ERR_INVAL;
    memcpy(out, &m.w[1], (size_t)count);
    return count;
}

static uint64_t size;
static char name[24];

static int mount_file(void)
{
    struct msg m = { .w = { BLK_SIZE } };
    if (sys_call(BLK, &m, 0) || m.w[0] < 3) return 1;
    uint64_t sectors = (uint64_t)m.w[0];
    uint64_t header[5];
    if (disk_read(0, 0, 24, header) < 0 || disk_read(0, 24, 16, &header[3]) < 0)
        return 1;
    memcpy(name, &header[2], sizeof name);
    int terminated = 0;
    for (int i = 0; i < 24; i++) if (!name[i]) terminated = 1;
    size = header[1];
    if (header[0] != TINYFS_MAGIC || !terminated || !name[0] || size > (sectors - 2) * 512)
        return 1;
    struct msg probe = { .w = { BLK_READ, (long)sectors, 0, 24 } };
    check(sys_call(BLK, &probe, 0) == 0 && probe.w[0] == -ERR_INVAL,
          "block server rejects reads beyond disk capacity");
    probe = (struct msg){ .w = { BLK_READ, 0, 500, 24 } };
    check(sys_call(BLK, &probe, 0) == 0 && probe.w[0] == -ERR_INVAL,
          "block server rejects chunks crossing a sector boundary");
    probe = (struct msg){ .w = { PROTO_SHUTDOWN } };
    check(sys_call(BLK, &probe, 0) == 0 && probe.w[0] == -ERR_PERM,
          "block server refuses client shutdown");
    printf("mounted %s (%lu bytes)\n", name, size);
    return 0;
}

int main(void)
{
    printf_use_console(SLOT_USES);
    int unavailable = mount_file();
    if (unavailable) printf("disk unavailable; serving I/O errors\n");
    struct msg m;
    if (sys_recv(SLOT_SERVE, &m)) return 1;
    for (;;) {
        long reply[4] = { -ERR_INVAL, 0, 0, 0 };
        int stop = 0;
        if (PROTO_OP(m.w[0]) == PROTO_SHUTDOWN) {
            reply[0] = m.badge ? -ERR_PERM : 0;
            stop = !m.badge;
        } else if (unavailable) {
            reply[0] = -ERR_IO;
        } else if (PROTO_OP(m.w[0]) == FS_STAT) {
            char query[25] = { 0 };
            memcpy(query, &m.w[1], 24);
            if (!strcmp(name, query)) { reply[0] = 1; reply[1] = (long)size; }
        } else if (PROTO_OP(m.w[0]) == FS_READ && m.w[1] == 1 && m.w[2] >= 0 &&
                   m.w[3] > 0 && m.w[3] <= STORAGE_CHUNK) {
            uint64_t offset = (uint64_t)m.w[2];
            long count = m.w[3];
            if (offset >= size) reply[0] = 0;
            else {
                if ((uint64_t)count > size - offset) count = (long)(size - offset);
                long first = count;
                if (first > 512 - (long)(offset % 512)) first = 512 - (long)(offset % 512);
                reply[0] = disk_read(1 + offset / 512, (long)(offset % 512), first, &reply[1]);
                if (reply[0] >= 0 && first < count) {
                    long r = disk_read(2 + offset / 512, 0, count - first,
                                       (char *)&reply[1] + first);
                    reply[0] = r < 0 ? r : count;
                }
            }
        }
        if (m.cap > 0) sys_cap_delete(m.cap);
        if (stop) { sys_reply(reply, 0); return failures ? 1 : 0; }
        if (sys_reply_recv(SLOT_SERVE, reply, 0, &m)) return 1;
    }
}
