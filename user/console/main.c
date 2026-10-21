/* The console server: the first program the root task starts from the
 * manifest. Clients send it text through badged capabilities; it collects
 * each client's text into lines and prints them, prefixed with the
 * client's badge.
 *
 * It still prints through SYS_DEBUG_WRITE, the kernel's serial output.
 * In M7 it gets the serial port's I/O capability and its interrupt and
 * drives the UART (universal asynchronous receiver-transmitter) itself. */
#include <stdint.h>
#include <abi/boot.h>
#include <abi/console.h>
#include "ipc.h"
#include "ulib.h"

#define LINE_MAX 120

static struct {
    char   text[LINE_MAX + 1];
    size_t len;
} lines[CONSOLE_MAX_CLIENTS];

static void flush(long badge)
{
    if (!lines[badge].len)
        return;
    lines[badge].text[lines[badge].len] = '\0';
    if (badge == 0)
        printf("root: %s\n", lines[badge].text);
    else
        printf("client %ld: %s\n", badge, lines[badge].text);
    lines[badge].len = 0;
}

static long write(long badge, long w0, const long *words)
{
    long len = (w0 >> 8) & 0xff;
    if (len > CONSOLE_WRITE_MAX)
        return -ERR_INVAL;
    if (badge < 0 || badge >= CONSOLE_MAX_CLIENTS)
        return -ERR_INVAL;   /* more clients than we have line buffers for */
    const char *bytes = (const char *)words;
    for (long i = 0; i < len; i++) {
        if (bytes[i] == '\n') {
            flush(badge);
            continue;
        }
        lines[badge].text[lines[badge].len++] = bytes[i];
        if (lines[badge].len == LINE_MAX)
            flush(badge);   /* an over-long line is split, not dropped */
    }
    return len;
}

int main(void)
{
    printf("ready\n");

    struct msg m;
    long r = sys_recv(SLOT_SERVE, &m);
    for (;;) {
        if (r) {
            printf("FAILED: receive returned %ld\n", r);
            return 1;
        }
        long reply[4] = { 0 };
        switch (PROTO_OP(m.w[0])) {
        case CONSOLE_WRITE:
            reply[0] = write(m.badge, m.w[0], &m.w[1]);
            break;

        case PROTO_SHUTDOWN:
            if (m.badge != 0) {   /* only the root task holds badge 0 */
                reply[0] = -ERR_PERM;
                break;
            }
            for (long b = 0; b < CONSOLE_MAX_CLIENTS; b++)
                flush(b);
            printf("shutting down\n");
            sys_reply(reply, 0);
            return 0;

        default:
            reply[0] = -ERR_NOSYS;
        }
        r = sys_reply_recv(SLOT_SERVE, reply, 0, &m);
    }
}