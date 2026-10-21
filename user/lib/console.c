#include <abi/console.h>
#include "console.h"
#include "ipc.h"
#include "ulib.h"

long console_write(long ep, const char *s, size_t len)
{
    while (len) {
        size_t n = len < CONSOLE_WRITE_MAX ? len : CONSOLE_WRITE_MAX;
        struct msg m = { 0 };
        m.w[0] = CONSOLE_WRITE | (long)(n << 8);
        memcpy(&m.w[1], s, n);   /* the text rides in words 1-3 */
        long r = sys_call(ep, &m, 0);
        if (r)
            return r;
        if (m.w[0] < 0)
            return m.w[0];
        s += n;
        len -= n;
    }
    return 0;
}

long console_puts(long ep, const char *s)
{
    return console_write(ep, s, strlen(s));
}