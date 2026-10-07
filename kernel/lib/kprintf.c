#include <stdint.h>
#include <stdbool.h>
#include <arch/serial.h>
#include <kernel/kprintf.h>

static void put_uint(uint64_t v, unsigned base, bool upper, int width, char pad)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[24];
    int i = 0;
    do {
        buf[i++] = digits[v % base];
        v /= base;
    } while (v);
    while (i < width && i < (int)sizeof buf)
        buf[i++] = pad;
    while (i)
        serial_putc(buf[--i]);
}

void kvprintf(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            serial_putc(*fmt);
            continue;
        }
        fmt++;

        char pad = ' ';
        int width = 0, lng = 0;
        bool left = false;
        if (*fmt == '-') {
            left = true;
            fmt++;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'z') {
            lng++;
            fmt++;
        }

        switch (*fmt) {
        case 'c':
            serial_putc((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = 0;
            while (s[len])
                len++;
            for (int i = len; !left && i < width; i++)
                serial_putc(' ');
            serial_write(s);
            for (int i = len; left && i < width; i++)
                serial_putc(' ');
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            if (v < 0) {
                serial_putc('-');
                v = -v;
            }
            put_uint((uint64_t)v, 10, false, width, pad);
            break;
        }
        case 'u':
            put_uint(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10,
                     false, width, pad);
            break;
        case 'x':
        case 'X':
            put_uint(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16,
                     *fmt == 'X', width, pad);
            break;
        case 'p':
            serial_write("0x");
            put_uint((uintptr_t)va_arg(ap, void *), 16, false, 16, '0');
            break;
        case '%':
            serial_putc('%');
            break;
        case '\0':
            return;
        default:   /* unknown conversion: print it verbatim */
            serial_putc('%');
            serial_putc(*fmt);
        }
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}