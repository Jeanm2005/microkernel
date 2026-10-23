#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include "console.h"
#include "syscall.h"
#include "ulib.h"

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int failures;

void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAILED", what);
    if (!ok)
        failures++;
}

/* ---- formatting ---------------------------------------------------------- */

struct out {
    char  *buf;
    size_t size, len;   /* len never exceeds size - 1 */
};

static void put(struct out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len++] = c;
}

static void put_uint(struct out *o, uint64_t v, unsigned base)
{
    char tmp[24];
    int i = 0;
    do {
        tmp[i++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (i)
        put(o, tmp[--i]);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { .buf = buf, .size = size, .len = 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;
        bool lng = false;
        if (*fmt == 'l') {
            lng = true;
            fmt++;
        }
        switch (*fmt) {
        case 'c':
            put(&o, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            for (s = s ? s : "(null)"; *s; s++)
                put(&o, *s);
            break;
        }
        case 'd': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            if (v < 0) {
                put(&o, '-');
                v = -v;
            }
            put_uint(&o, (uint64_t)v, 10);
            break;
        }
        case 'u':
            put_uint(&o, lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10);
            break;
        case 'x':
            put_uint(&o, lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16);
            break;
        case 'p':
            put(&o, '0');
            put(&o, 'x');
            put_uint(&o, (uint64_t)va_arg(ap, void *), 16);
            break;
        case '%':
            put(&o, '%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            put(&o, '%');
            put(&o, *fmt);
        }
    }
    if (size)
        buf[o.len] = '\0';
    return (int)o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- printf -------------------------------------------------------------- */

static long console_ep;

void printf_use_console(long ep)
{
    console_ep = ep;
}

int printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (console_ep)
        console_write(console_ep, buf, (size_t)n);
    else
        sys_debug_write(buf, (size_t)n);
    return n;
}
